#include "ota.h"
#include "main.h"
#include "w25qxx.h"
#include "crc32.h"
#include "flash_if.h"
#include <string.h>
#include <stdio.h>

extern UART_HandleTypeDef huart1;   /* main.c 里的串口句柄 */

/* ============ 参数区结构体（存于最后一页 1KB）============ */
typedef struct {
    uint32_t magic;   /* OTA_MAGIC */
    uint32_t state;   /* OTA_STATE_* */
    uint32_t size;    /* 固件字节数 */
    uint32_t crc;     /* 固件 CRC32 */
} ota_param_t;        /* 共 16 字节，远小于 1KB 页 */

/* ---------- 参数读写（直接映射 Flash 地址）---------- */
static void param_read(ota_param_t *p)
{
    memcpy(p, (const void *)PARAM_START, sizeof(ota_param_t));
}
static void param_write(const ota_param_t *p)
{
    FLASH_If_Init();
    FLASH_If_Erase(PARAM_START, PARAM_START + sizeof(ota_param_t));
    FLASH_If_Write(PARAM_START, (const uint8_t *)p, sizeof(ota_param_t));
    FLASH_If_Lock();
}

/* ---------- 判断 APP 区是否装了合法程序 ---------- */
static int app_valid(void)
{
    uint32_t sp = *(volatile uint32_t *)APP_START;        /* 复位向量第1字 = 主栈指针 */
    uint32_t pc = *(volatile uint32_t *)(APP_START + 4);  /* 复位向量第2字 = 复位入口 */
    if (sp < 0x20000000UL || sp > 0x20005000UL) return 0; /* 栈必须在 SRAM 范围内 */
    if (pc < APP_START || pc > (APP_START + APP_MAX_SIZE)) return 0;
    return 1;
}

/* ---------- 七步跳转法：从 Boot 跳到 APP ---------- */
static void jump_to_app(void)
{
    uint32_t sp = *(volatile uint32_t *)APP_START;
    uint32_t pc = *(volatile uint32_t *)(APP_START + 4);

    printf("[BOOT] jump to APP @0x%08lX\r\n", (unsigned long)APP_START);

    __disable_irq();                          /* ① 关全局中断 */
    SysTick->CTRL = 0; SysTick->LOAD = 0;     /* ② 停 SysTick */
    SysTick->VAL  = 0;
    HAL_RCC_DeInit();                         /* ③ 复位时钟（APP 会重新配置） */
    HAL_DeInit();                             /* ④ 复位 HAL 外设状态 */
    __set_MSP(sp);                            /* ⑤ 把 MSP 切到 APP 的栈顶 */
    /* ⑥ 清 NVIC  pending（简单起见此处省略逐位清，HAL_DeInit 已处理大部分） */
    void (*app_reset)(void) = (void (*)(void))pc;  /* ⑦ 函数指针跳 PC */
    app_reset();
    while (1);   /* 永不返回 */
}

/* ---------- 把 W25Q 暂存的固件搬到内部 Flash 的 APP 区并 CRC 校验 ---------- */
static int copy_to_app(uint32_t size, uint32_t expected_crc)
{
    FLASH_If_Init();
    FLASH_If_Erase(APP_START, APP_START + size);  /* 先整片擦 APP 区 */

    uint8_t  buf[256];
    crc32_init();
    uint32_t crc = crc32_start();
    uint32_t off = 0;
    while (off < size)
    {
        uint32_t n = (size - off < 256) ? (size - off) : 256;
        W25Q_Read(off, buf, n);                    /* 从 W25Q 读一包 */
        if (FLASH_If_Write(APP_START + off, buf, n) != FLASH_IF_OK)
        {
            FLASH_If_Lock();
            return -1;                             /* 写失败 */
        }
        crc = crc32_step(crc, buf, n);             /* 边搬边算 CRC */
        off += n;
    }
    crc = crc32_finish(crc);
    FLASH_If_Lock();

    if (crc != expected_crc)
    {
        printf("[BOOT] CRC mismatch calc=%08lX exp=%08lX\r\n",
               (unsigned long)crc, (unsigned long)expected_crc);
        return -2;                                 /* 校验失败 */
    }
    return 0;
}

/* ---------- 串口收 1 字节（带超时 ms；timeout=0xFFFFFFFF 表示永久阻塞）----------
   V2 加固：溢出(ORE)/噪声(NE)/帧错误(FE)/校验错误(PE) 一旦置位，
   如果不清除，HAL_UART_Receive 之后会永久返回失败（这就是"传一半突然静默"的元凶）。 */
static int uart_getc(uint8_t *c, uint32_t timeout)
{
    HAL_StatusTypeDef st = HAL_UART_Receive(&huart1, c, 1, timeout);

    if (st == HAL_OK) return 1;

    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_ORE) ||
        __HAL_UART_GET_FLAG(&huart1, UART_FLAG_NE)  ||
        __HAL_UART_GET_FLAG(&huart1, UART_FLAG_FE)  ||
        __HAL_UART_GET_FLAG(&huart1, UART_FLAG_PE))
    {
        __HAL_UART_CLEAR_OREFLAG(&huart1);           /* 读 SR 再读 DR，清掉全部错误标志 */
        huart1.ErrorCode = HAL_UART_ERROR_NONE;
    }
    huart1.RxState = HAL_UART_STATE_READY;           /* 兜底：把接收状态机拉回可用 */
    return 0;
}
static void uart_puts(const char *s)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)s, strlen(s), 0xFFFF);
}

/* ---------- 把接收缓冲清空，直到线路安静 quiet_ms 毫秒 ----------
   帧出错后必须先"扔掉"错误帧的残余字节，
   否则下一次解析会把残渣当成新帧头，永远对不上。 */
static void uart_drain(uint32_t quiet_ms)
{
    uint8_t c;
    while (uart_getc(&c, quiet_ms)) { }   /* 一直读到连续 quiet_ms 没有新数据 */
}

/* ---------- 校验 W25Q 里暂存的固件 CRC 是否与 PC 端一致 ---------- */
static int verify_w25q(uint32_t size, uint32_t expected_crc)
{
    uint8_t  buf[256];
    uint32_t calc, off = 0, n;

    crc32_init();
    calc = crc32_start();
    while (off < size)
    {
        n = (size - off < 256) ? (size - off) : 256;
        W25Q_Read(off, buf, n);
        calc = crc32_step(calc, buf, n);
        off += n;
    }
    calc = crc32_finish(calc);

    if (calc == expected_crc) return 1;

    printf("[BOOT] VERIFIED FAIL calc=%08lX exp=%08lX\r\n",
           (unsigned long)calc, (unsigned long)expected_crc);
    return 0;
}

/* ---------- 升级模式：接收固件 -> 暂存 W25Q -> 校验 -> 写参数 -> 复位 ----------
   V2 加固：整个流程包在一个"永不退出"的大循环里。
   任何一帧出错（超时/线路抖动/CRC 不符）都只是回到"重新等 START"，
   PC 端原地重发 START 就能继续，不必再按复位键重启板子。 */
static void enter_upgrade(void)
{
    char line[64];
    uart_puts("[BOOT] upgrade mode. send: START:<size>:<crc32>\r\n");

    while (1)                                   /* 大循环：失败就重来 */
    {
        uint32_t size = 0, crc = 0;
        int i = 0;

        /* 1) 读一行文本，直到解析出合法 START（其余内容一律忽略） */
        while (1)
        {
            uint8_t c;
            if (!uart_getc(&c, 0xFFFFFFFF)) continue;      /* 阻塞等 */
            if (c == '\r' || c == '\n')
            {
                line[i] = 0;
                if (i > 0 && sscanf(line, "START:%lu:%x", &size, &crc) == 2)
                    break;                                 /* 解析成功 */
                i = 0;
                continue;
            }
            if (i < (int)sizeof(line) - 1) line[i++] = (char)c;
            else i = 0;                                    /* 行太长：丢弃重来 */
        }
        printf("[BOOT] recv firmware size=%lu crc=%08lX\r\n",
               (unsigned long)size, (unsigned long)crc);

        /* 2) 擦除 W25Q 暂存区（按 4KB 扇区） */
        {
            uint32_t sectors = (size + 4095) / 4096;
            for (uint32_t s = 0; s < sectors; s++)
                W25Q_SectorErase(s * 4096);
        }
        uart_puts("READY\r\n");

        /* 3) 收二进制帧：[offset 4B 大端][len 1B][data len]
              帧级重传：某帧坏了只要求 PC 重发这一帧，不影响已传部分
              （每帧自带 offset，天然可重发，这是最省事的可靠性设计） */
        {
            int      frame_err = 0;
            int      restart   = 0;
            uint32_t good_frames = 0;

            while (1)
            {
                uint8_t  hdr[5] = {0,0,0,0,0};
                uint8_t  buf[128];
                uint32_t offset = 0;
                uint8_t  len = 0;
                int      k, ok = 1;

                for (k = 0; k < 5; k++)                    /* 帧头 5 字节 */
                    if (!uart_getc(&hdr[k], 5000)) { ok = 0; break; }

                if (ok)
                {
                    offset = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                             ((uint32_t)hdr[2] << 8)  |  (uint32_t)hdr[3];
                    len    = hdr[4];

                    if (offset == 0xFFFFFFFFUL && len == 0) break;  /* 结束帧 */

                    /* 合法性检查：offset 必须落在固件范围内、len ≤ 128 */
                    if (offset >= size || len == 0 || len > 128) ok = 0;
                }

                if (ok)
                {
                    for (k = 0; k < len; k++)              /* 帧数据 */
                        if (!uart_getc(&buf[k], 2000)) { ok = 0; break; }
                }

                if (!ok)
                {
                    /* 帧损坏/超时：通知 PC 重发，清空线路后继续等下一帧头 */
                    frame_err++;
                    printf("[BOOT] err#%d hdr=%02X %02X %02X %02X %02X\r\n",
                           frame_err, hdr[0], hdr[1], hdr[2], hdr[3], hdr[4]);
                    uart_drain(100);                       /* 扔干净残余字节 */
                    if (frame_err > 20)                    /* 错太多次：回 START 重来 */
                    {
                        uart_puts("[BOOT] too many errors, restart from START\r\n");
                        restart = 1;
                        break;
                    }
                    continue;
                }

                W25Q_PageProgram(offset, buf, len);        /* 写一帧到 W25Q */
                uart_puts("A");                            /* 应答 */
                frame_err = 0;
                good_frames++;
            }

            if (restart) continue;         /* 回大循环，重新等 START */
            if (good_frames == 0) continue;
        }

        /* 4) 整包 CRC 校验 */
        if (verify_w25q(size, crc))
        {
            ota_param_t p = { OTA_MAGIC, OTA_STATE_PENDING, size, crc };
            param_write(&p);                      /* 记下"待升级"，等复位后搬移 */
            uart_puts("\r\n[BOOT] VERIFIED OK, reboot to flash\r\n");
            HAL_Delay(200);
            NVIC_SystemReset();                   /* 软复位，重新进 Boot 触发搬移 */
        }
        else
        {
            uart_puts("[BOOT] resend START to retry\r\n");
            continue;                             /* 校验失败也回到等 START */
        }
    }
}

/* ============ 升级主入口 ============ */
void ota_boot(void)
{
    /* ① 上电先看参数区：有 PENDING 就搬固件进 APP 区 */
    ota_param_t p;
    param_read(&p);
    if (p.magic == OTA_MAGIC && p.state == OTA_STATE_PENDING)
    {
        printf("[BOOT] pending firmware found, size=%lu\r\n", (unsigned long)p.size);
        if (copy_to_app(p.size, p.crc) == 0)
        {
            p.state = OTA_STATE_CONFIRMED;    /* 标记已落地，下次正常跳 APP */
            param_write(&p);
            jump_to_app();
        }
        else
        {
            uart_puts("[BOOT] copy failed, stay in boot\r\n");
        }
    }

    /* ② 2 秒窗口：收到任意字符 -> 升级模式；否则直接跳 APP */
    uart_puts("[BOOT] press any key in 2s to upgrade, else run APP\r\n");
    uint8_t k;
    if (uart_getc(&k, 2000))
    {
        enter_upgrade();
    }
    else if (app_valid())
    {
        jump_to_app();
    }
    else
    {
        uart_puts("[BOOT] no APP, enter upgrade\r\n");
        enter_upgrade();
    }
}
