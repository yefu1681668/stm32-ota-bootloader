#ifndef FLASH_IF_H
#define FLASH_IF_H

#include <stdint.h>

/* ============ C8T6 内部 Flash 分区（64KB，1KB/页，共 64 页）============
   0x08000000 +-------------------+  <- 芯片 Flash 基地址
              |  BOOT  (16KB)     |     放 BootLoader 自身
   0x08004000 +-------------------+  <- APP 起始（和课程一致）
              |  APP   (47KB)     |     放应用固件
   0x0800FC00 +-------------------+  <- 参数区（最后 1 页）
              |  PARAM (1KB)      |     存升级标志/固件大小/CRC
   0x08010000 +-------------------+  <- 64KB 结束
   护栏说明：Boot 的 IROM1 只开 0x4000(16KB)，链接器会在超界时直接报错，
   防止 Boot 代码不小心踩到 APP 区。 */
#define FLASH_PAGE_SIZE   0x400U        /* F103 中密度：每页 1KB */
#define FLASH_BASE_ADDR   0x08000000U
#define FLASH_TOTAL_PAGES 64U

#define BOOT_START        0x08000000U
#define BOOT_SIZE         0x4000U       /* 16KB */
#define APP_START         0x08004000U   /* APP 起始地址 */
#define APP_MAX_SIZE      0xBC00U       /* 47KB */
#define PARAM_START       0x0800FC00U   /* 参数区：最后一页 */
#define PARAM_SIZE        0x400U

/* 错误码 */
#define FLASH_IF_OK         0
#define FLASH_IF_ERR_ERASE  1
#define FLASH_IF_ERR_WRITE  2
#define FLASH_IF_ERR_ALIGN  3

int  FLASH_If_Init(void);                         /* 解锁 Flash（擦/写前必须） */
int  FLASH_If_Erase(uint32_t start, uint32_t end);/* 擦掉 [start,end) 覆盖到的整页 */
int  FLASH_If_Write(uint32_t dst,
                    const uint8_t *src,
                    uint32_t len);                /* 写（F1 半字编程，len 必须偶数） */
int  FLASH_If_Verify(uint32_t dst,
                     const uint8_t *src,
                     uint32_t len);               /* 回读比对，返回 0=一致 */
void FLASH_If_Lock(void);                         /* 上锁（完事务必调用） */

#endif /* FLASH_IF_H */
