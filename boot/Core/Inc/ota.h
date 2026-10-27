#ifndef OTA_H
#define OTA_H

#include <stdint.h>

/* 参数区“升级状态”取值 */
#define OTA_MAGIC          0xDEADBEEFU   /* 魔数：确认参数区是我们的格式 */
#define OTA_STATE_NONE      0U           /* 无待升级固件 */
#define OTA_STATE_PENDING   1U           /* 固件已暂存 W25Q，待搬到 APP 区 */
#define OTA_STATE_CONFIRMED 2U           /* 已成功写入 APP 区，可正常跳转 */

/* 升级主入口：上电由 main() 调用。
   内部逻辑：
     1) 若参数区有 PENDING -> 把 W25Q 固件搬到内部 Flash -> 跳 APP
     2) 否则开 2 秒窗口：收到任意字符进“升级模式”，否则直接跳 APP
     3) 无 APP 时进入升级模式等待上位机发固件 */
void ota_boot(void);

#endif /* OTA_H */
