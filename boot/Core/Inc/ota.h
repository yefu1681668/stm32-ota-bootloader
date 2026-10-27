#ifndef OTA_H
#define OTA_H

#include "ota_param.h"   /* 参数区结构 + 升级状态定义（Boot/APP 共用） */

/* 升级主入口：上电由 main() 调用。内部逻辑：
     1) 若参数区有 PENDING -> 把 W25Q 固件搬到内部 Flash -> 跳 APP
     2) 若 state==VALID 且 try_count<MAX -> 累加计数后跳 APP；>=MAX 则停 Boot 等重刷
     3) 若 state==APP_OK -> 直接跳 APP
     4) 否则开 2 秒窗口：收到任意字符进升级模式；无合法 APP 则进升级模式等待 */
void ota_boot(void);

#endif /* OTA_H */
