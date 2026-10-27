#ifndef OTA_PARAM_H
#define OTA_PARAM_H

#include <stdint.h>
#include "flash_if.h"   /* PARAM_START 等地址来自这里 */

/* ============ 参数区“升级状态”取值 ============
   NONE    : 未初始化 / 无合法固件
   PENDING : 固件已暂存 W25Q，待软复位后搬到 APP 区
   VALID   : 已搬入 APP 区，但 APP 还没回写“我活着”
   APP_OK  : APP 已自确认存活（连续运行成功）           */
#define OTA_MAGIC          0xDEADBEEFU
#define OTA_STATE_NONE      0U
#define OTA_STATE_PENDING   1U
#define OTA_STATE_VALID     2U
#define OTA_STATE_APP_OK    3U

/* APP 连续起不来的上限：try_count 累加到这个数就停 Boot 等重刷 */
#define OTA_MAX_TRY         3U

/* ============ 参数区结构体（存于最后一页 1KB，地址 PARAM_START）============
   注意：Boot 与 APP 必须共用同一份布局，字段顺序/大小改变要两端同步！ */
typedef struct {
    uint32_t magic;      /* OTA_MAGIC */
    uint32_t state;      /* OTA_STATE_* */
    uint32_t size;       /* 固件字节数 */
    uint32_t crc;        /* 固件 CRC32 */
    uint32_t try_count;  /* Boot 累计跳转 APP 次数；APP 自确认后清零 */
} ota_param_t;           /* 共 20 字节，远小于 1KB 页 */

#endif /* OTA_PARAM_H */
