/**
  ******************************************************************************
  * @file    can.c
  * @brief   bxCAN 驱动 + 自环自检（单块板也能验证 CAN 链路）
  ******************************************************************************
  * 为什么一块板也能测：
  *   CAN_BTR 里的 LBKM 位 = 1 时，CAN 控制器内部把"发送"直接接到"接收"，
  *   不依赖 CAN_RX 引脚、也不需要外部节点回 ACK。
  *   所以没有第二块板、甚至没接收发器，也能把驱动/过滤器/分帧逻辑跑通。
  *
  * 切到真实总线只需把 can_init(CAN_MODE_LOOPBACK) 改成 can_init(CAN_MODE_NORMAL)。
  ******************************************************************************
  */
#include "can.h"
#include <stdio.h>
#include <string.h>

CAN_HandleTypeDef hcan;

/* ----------------------------------------------------------------------------
 * 500kbps 波特率是怎么算出来的（C8T6 主频 72MHz，APB1 = 36MHz）
 *
 *   一个位 = 1 个同步段 + TS1 + TS2
 *   波特率 = APB1 / (Prescaler × (1 + TS1 + TS2))
 *
 *   取 Prescaler = 4 -> 36MHz / 4 = 9MHz（每个时间片 111ns）
 *   取 TS1 = 15、TS2 = 2 -> 一位 = 18 个时间片
 *   9MHz / 18 = 500kHz  ✓
 *
 *   采样点 = (1 + 15) / 18 = 88.9%（标准建议 87.5%，非常接近，通信稳定）
 * -------------------------------------------------------------------------- */
#define CAN_PRESCALER      4U
#define CAN_TS1            CAN_BS1_15TQ
#define CAN_TS2            CAN_BS2_2TQ
#define CAN_SJW            CAN_SJW_1TQ

/**
  * @brief  初始化 CAN1
  * @param  mode: CAN_MODE_NORMAL(真实总线) / CAN_MODE_LOOPBACK(自环自测)
  * @retval CAN_OK / CAN_ERR_INIT
  */
uint8_t can_init(uint32_t mode)
{
    CAN_FilterTypeDef f = {0};

    /* 重复调用时先反初始化，否则第二次 HAL_CAN_Init 会报状态错误 */
    if (hcan.State != HAL_CAN_STATE_RESET)
    {
        HAL_CAN_DeInit(&hcan);
    }

    hcan.Instance                  = CAN1;
    hcan.Init.Prescaler            = CAN_PRESCALER;
    hcan.Init.Mode                 = mode;
    hcan.Init.SyncJumpWidth        = CAN_SJW;
    hcan.Init.TimeSeg1             = CAN_TS1;
    hcan.Init.TimeSeg2             = CAN_TS2;
    hcan.Init.TimeTriggeredMode    = DISABLE;   /* 经典 CAN，不做时间触发 */
    hcan.Init.AutoBusOff           = DISABLE;   /* 总线关闭后由软件决定怎么恢复 */
    hcan.Init.AutoWakeUp           = DISABLE;
    hcan.Init.AutoRetransmission   = ENABLE;    /* 发失败自动重发（CAN 的可靠性来源） */
    hcan.Init.ReceiveFifoLocked    = DISABLE;   /* FIFO 满了覆盖旧帧，不锁死 */
    hcan.Init.TransmitFifoPriority = DISABLE;   /* 按 ID 优先级仲裁发送 */

    if (HAL_CAN_Init(&hcan) != HAL_OK)
    {
        return CAN_ERR_INIT;
    }

    /* 过滤器：32 位掩码模式，掩码全 0 = 全部放行（教学阶段先全收，后面再收紧） */
    f.FilterBank           = 0;
    f.FilterMode           = CAN_FILTERMODE_IDMASK;
    f.FilterScale          = CAN_FILTERSCALE_32BIT;
    f.FilterIdHigh         = 0x0000;
    f.FilterIdLow          = 0x0000;
    f.FilterMaskIdHigh     = 0x0000;            /* 掩码位 0 = 这一位不关心 */
    f.FilterMaskIdLow      = 0x0000;
    f.FilterFIFOAssignment = CAN_RX_FIFO0;
    f.FilterActivation     = ENABLE;
    f.SlaveStartFilterBank = 14;                /* 单 CAN 的芯片这个值随便填 */

    if (HAL_CAN_ConfigFilter(&hcan, &f) != HAL_OK)
    {
        return CAN_ERR_INIT;
    }

    if (HAL_CAN_Start(&hcan) != HAL_OK)         /* 从初始化态进入正常态，才能收发 */
    {
        return CAN_ERR_INIT;
    }

    return CAN_OK;
}

/**
  * @brief  发送一帧标准数据帧
  * @param  id: 11 位标准 ID
  * @param  data: 数据指针
  * @param  len: 长度 0~8
  * @retval CAN_OK / CAN_ERR_SEND
  */
uint8_t can_send(uint32_t id, const uint8_t *data, uint8_t len)
{
    CAN_TxHeaderTypeDef hdr;
    uint32_t mailbox = 0;
    uint32_t t0;

    if (len > 8U) len = 8U;

    hdr.StdId              = id;
    hdr.ExtId              = 0;
    hdr.IDE                = CAN_ID_STD;        /* 标准帧（11 位 ID） */
    hdr.RTR                = CAN_RTR_DATA;      /* 数据帧，不是远程帧 */
    hdr.DLC                = len;
    hdr.TransmitGlobalTime = DISABLE;

    /* 等一个空闲发送邮箱（一共 3 个），满了说明总线堵了 */
    t0 = HAL_GetTick();
    while (HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 0U)
    {
        if (HAL_GetTick() - t0 > 50U) return CAN_ERR_SEND;
    }

    if (HAL_CAN_AddTxMessage(&hcan, &hdr, (uint8_t *)data, &mailbox) != HAL_OK)
    {
        return CAN_ERR_SEND;
    }

    /* 等这帧真正发出去（邮箱标志被硬件置位） */
    t0 = HAL_GetTick();
    while (HAL_CAN_IsTxMessagePending(&hcan, mailbox) != 0U)
    {
        if (HAL_GetTick() - t0 > 50U) return CAN_ERR_SEND;
    }

    return CAN_OK;
}

/**
  * @brief  从 FIFO0 收一帧（轮询版，本阶段不用中断，简单可靠）
  * @param  id:  输出，收到的标准 ID
  * @param  data: 输出缓冲，至少 8 字节
  * @param  len: 输出，实际长度
  * @param  timeout_ms: 超时毫秒
  * @retval CAN_OK / CAN_ERR_RECV(超时)
  */
uint8_t can_recv(uint32_t *id, uint8_t *data, uint8_t *len, uint32_t timeout_ms)
{
    CAN_RxHeaderTypeDef rxh;
    uint32_t t0 = HAL_GetTick();

    while (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) == 0U)
    {
        if (HAL_GetTick() - t0 > timeout_ms) return CAN_ERR_RECV;
    }

    if (HAL_CAN_GetRxMessage(&hcan, CAN_RX_FIFO0, &rxh, data) != HAL_OK)
    {
        return CAN_ERR_RECV;
    }

    *id  = rxh.StdId;
    *len = (uint8_t)rxh.DLC;
    return CAN_OK;
}

/* ----------------------------------------------------------------------------
 * 自检：分三步，一步步把问题范围缩小
 *   T1 单帧 6 字节回环     —— 软件自环，验 ID / 长度 / 数据
 *   T2 200 字节分帧重组    —— 软件自环，验将来的固件传输分帧逻辑
 *   T3 物理回环（NORMAL）  —— 真实总线模式，收发器把信号打上 CANH/CANL
 *                             再自己收回来，用来自证"接线 + 收发器 + 终端电阻"
 * 返回 0 = 全通过，其它值 = 卡在第几步（方便定位）
 *   [pin check] 先打印 PA11 电平：LOW 说明收发器没在驱动这根线
 *   [pin drive] PA12 拉低时 PA11 是否跟随：不跟随说明 CTX 这根线/PA12 引脚有问题
 *   1     : CAN 初始化失败
 *   2~4   : T1 发/收/数据不符   -> MCU 内部 CAN 控制器问题
 *   5~8   : T2 分帧重组问题
 *   9     : T3 初始化失败
 *   20    : 收发器/接线通路不通（drive test 失败）
 * 注意：T3 用 NORMAL 模式单节点发帧，拿不到 ACK 属正常现象，不参与判定。
 * -------------------------------------------------------------------------- */
/**
  * @brief  引脚静态检查：判断 PA11 是否真的接到了收发器的 R(CRX) 输出
  *
  *   把 PA11 配成"普通 GPIO 输入 + 内部下拉"，读它的电平：
  *     读到 1 -> 外部有器件把它拉高（收发器 R 在隐性状态输出高电平）
  *               => 模块有 3.3V 供电、CRX 确实连到了 PA11
  *     读到 0 -> 内部下拉赢了，说明这根线没人驱动
  *               => 模块没供电 / CRX 没接上 / PA11 接到了模块的输入脚(CTX 接反)
  *
  *   这一步在 CAN 外设初始化之前做，PA11 此时只是普通 GPIO，不影响后面 can_init。
  */
static uint8_t can_pin_check(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    g.Pin  = GPIO_PIN_11;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLDOWN;             /* 内部下拉：没人驱动就读 0 */
    HAL_GPIO_Init(GPIOA, &g);

    HAL_Delay(2);                       /* 等电平稳定 */
    return (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_11) == GPIO_PIN_SET) ? 1U : 0U;
}

/**
  * @brief  驱动通路测试：PA12 翻转，看 PA11 是否跟随
  *
  *   PA12 配成推挽输出当"假 CAN_TX"，PA11 配成输入读"假 CAN_RX"：
  *     PA12 = 高（隐性） -> 总线隐性 -> R 输出高 -> PA11 应为高
  *     PA12 = 低（显性） -> 总线显性 -> R 输出低 -> PA11 应为低
  *
  *   若两步都对 => 收发器驱动器工作、CTX 接线正常、CANH/CANL 引脚无损
  *   若 PA11 一直停在低   => 收发器没供电或根本没接
  *   若 PA11 一直停在高   => 驱动器没使能（Rs 悬空进 standby）或 PA12->CTX 没接通
  *
 *   返回 1 = 跟随成功；0 = 不跟随
 *   *rd_low 输出：PA12 被拉低时回读到的电平（0=正常，1=外部有东西在往上拉）
 */
static uint8_t can_pin_drive_test(uint8_t *rd_low)
{
    GPIO_InitTypeDef g = {0};
    uint8_t follows = 0;

    __HAL_RCC_GPIOA_CLK_ENABLE();

    g.Pin  = GPIO_PIN_11;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOA, &g);

    g.Pin   = GPIO_PIN_12;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &g);

    /* ① 隐性电平 */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_SET);
    HAL_Delay(2);
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_11) == GPIO_PIN_SET)
    {
        /* ② 显性电平 */
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_RESET);
        HAL_Delay(2);
        /* 回读 PA12 本身：推挽拉低时读到 1，说明外部有强上拉（接错到 3.3V / 短路） */
        *rd_low = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_12) == GPIO_PIN_SET) ? 1U : 0U;
        if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_11) == GPIO_PIN_RESET)
        {
            follows = 1;
        }
    }
    else
    {
        *rd_low = 0U;
    }

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_SET);   /* 释放总线 */
    return follows;
}

int can_selftest(void)
{
    uint8_t  tx[6];
    uint8_t  rx[8];
    uint8_t  payload[200];
    uint8_t  rebuild[200];
    uint32_t id;
    uint8_t  len;
    uint16_t off;
    uint16_t n;
    uint16_t i;
    uint8_t  hw_ok = 1U;        /* 收发器/接线是否通（drive test 的结果） */

    /* ---------- 先做引脚静态检查（判断收发器到底在不在线） ---------- */
    printf("[CAN] pin check PA11 ... ");
    if (can_pin_check())
    {
        printf("HIGH (transceiver drives the line: VCC + CRX OK)\r\n");
    }
    else
    {
        printf("LOW  (transceiver not driving: check 3.3V / common GND / CTX-CRX)\r\n");
    }

    /* ---------- 驱动通路测试：PA12 翻转，PA11 是否跟随 ---------- */
    {
        uint8_t rd_low = 0U;

        printf("[CAN] pin drive test ... ");
        if (can_pin_drive_test(&rd_low))
        {
            printf("PA11 follows PA12 (driver + CTX wire OK)\r\n");
        }
        else
        {
            printf("PA11 stuck\r\n");
            hw_ok = 0U;

            /* 用 PA12 回读区分两种失败原因 */
            if (rd_low)
            {
                printf("[CAN]   PA12 readback=1 while driven LOW: external pull-up\r\n");
                printf("[CAN]   => PA12 wire is on 3.3V or shorted, NOT on module TX\r\n");
            }
            else
            {
                printf("[CAN]   PA12 readback=0, MCU output is fine\r\n");
                printf("[CAN]   => module TX not reached, chip in standby, or CANH/CANL shorted\r\n");
                printf("[CAN]   => try the 2nd module; measure CANH/CANL to GND (~1.6V idle)\r\n");
            }
        }
    }

    /* ---------- 切到自环模式 ---------- */
    printf("[CAN] init 500kbps (LOOPBACK) ... ");
    if (can_init(CAN_MODE_LOOPBACK) != CAN_OK)
    {
        printf("FAIL\r\n");
        return 1;
    }
    printf("OK\r\n");

    /* ---------- 测试1：单帧回环 ---------- */
    for (i = 0; i < 6U; i++) tx[i] = (uint8_t)(0xA0U + i);

    if (can_send(CAN_ID_DATA, tx, 6) != CAN_OK)
    {
        printf("[CAN] T1 send FAIL\r\n");
        return 2;
    }
    if (can_recv(&id, rx, &len, 200) != CAN_OK)
    {
        printf("[CAN] T1 recv FAIL (timeout)\r\n");
        return 3;
    }
    if ((id != CAN_ID_DATA) || (len != 6U) || (memcmp(tx, rx, 6) != 0))
    {
        printf("[CAN] T1 mismatch: id=0x%03lX len=%u\r\n", (unsigned long)id, len);
        return 4;
    }
    printf("[CAN] T1 pass: 6B frame loopback OK\r\n");

    /* ---------- 测试2：200 字节分帧 -> 重组 ---------- */
    for (i = 0; i < 200U; i++)
    {
        payload[i] = (uint8_t)(i * 7U + 3U);    /* 造一组有规律但不像全 0 的数据 */
    }

    off = 0;
    while (off < 200U)
    {
        n = (uint16_t)(200U - off);
        if (n > CAN_DATA_PER_FRAME) n = CAN_DATA_PER_FRAME;

        if (can_send(CAN_ID_DATA, &payload[off], (uint8_t)n) != CAN_OK)
        {
            printf("[CAN] T2 send FAIL @%u\r\n", off);
            return 5;
        }
        if (can_recv(&id, rx, &len, 100) != CAN_OK)
        {
            printf("[CAN] T2 recv FAIL @%u\r\n", off);
            return 6;
        }
        if ((id != CAN_ID_DATA) || (len != (uint8_t)n))
        {
            printf("[CAN] T2 frame mismatch @%u\r\n", off);
            return 7;
        }

        memcpy(&rebuild[off], rx, n);
        off = (uint16_t)(off + n);
    }

    if (memcmp(payload, rebuild, 200U) != 0)
    {
        printf("[CAN] T2 data mismatch\r\n");
        return 8;
    }
    printf("[CAN] T2 pass: 200B / %u frames reassembled OK\r\n",
           (unsigned)((200U + CAN_DATA_PER_FRAME - 1U) / CAN_DATA_PER_FRAME));

    /* ---------- 测试3：NORMAL 模式发一帧（只作参考，不算失败） ----------
       重要概念：CAN 的 ACK 位必须由"另一个节点"在 ACK 时隙打显性。
       总线上只有自己一个节点时，这帧永远拿不到 ACK，控制器会一直重发
       （TEC 累加到 255 进 bus-off）。所以单板上 NORMAL 模式必然发不出去，
       这跟接线对不对无关——真正的"双节点总线测试"要等第二块板。
       因此这里只打印现象，不参与 PASS/FAIL 判定。 */
    printf("[CAN] T3 normal-mode TX (single node, expect no ACK) ... ");
    if (can_init(CAN_MODE_NORMAL) != CAN_OK)
    {
        printf("init FAIL\r\n");
        return 9;
    }

    for (i = 0; i < 6U; i++) tx[i] = (uint8_t)(0x5AU + i);

    if (can_send(CAN_ID_DATA, tx, 6) == CAN_OK)
    {
        printf("OK (a 2nd node answered ACK: real bus works!)\r\n");
    }
    else
    {
        printf("no ACK, as expected (need 2nd node)\r\n");
    }

    /* 回到自环模式，让 APP 后续仍可安全使用 CAN */
    (void)can_init(CAN_MODE_LOOPBACK);

    if (hw_ok)
    {
        printf("[CAN] self-test PASS: MCU side + transceiver path OK\r\n");
        return 0;
    }
    return 20;      /* 20 = 收发器/接线通路不通 */
}
