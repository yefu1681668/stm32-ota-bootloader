/**
  ******************************************************************************
  * @file    can.h
  * @brief   bxCAN 驱动（STM32F103C8T6 内置 CAN1）+ 自环自检
  ******************************************************************************
  * 硬件连接（C8T6 <-> SN65HVD230 模块）：
  *     PA12 (CAN_TX)  ->  模块 CTX / D   驱动器输入  ← 同名相接，不是交叉！
  *     PA11 (CAN_RX)  <-  模块 CRX / R   接收器输出
  *     3.3V           ->  模块 VCC
  *     GND            ->  模块 GND
  *     模块 CANH / CANL -> 双绞线到另一个节点（两端各并 120Ω）
  ******************************************************************************
  */
#ifndef __CAN_H
#define __CAN_H

#include "main.h"

/* ---------- 报文 ID（与串口 OTA 阶段定的协议一一对应） ---------- */
#define CAN_ID_REQ      0x100U      /* 上位机 -> Boot：请求升级 */
#define CAN_ID_HEAD     0x101U      /* 上位机 -> Boot：固件头(长度/CRC) */
#define CAN_ID_ACK      0x102U      /* Boot -> 上位机：应答 */
#define CAN_ID_ERR      0x103U      /* Boot -> 上位机：错误码 */
#define CAN_ID_DONE     0x104U      /* 双向：升级结束 */
#define CAN_ID_DATA     0x201U      /* 上位机 -> Boot：固件数据（每帧 6 字节） */

#define CAN_DATA_PER_FRAME  6U      /* 每帧有效载荷（留 2 字节给序号/校验） */

/* ---------- 返回值 ---------- */
#define CAN_OK          0
#define CAN_ERR_INIT    1
#define CAN_ERR_SEND    2
#define CAN_ERR_RECV    3

extern CAN_HandleTypeDef hcan;

uint8_t can_init(uint32_t mode);                                  /* CAN_MODE_NORMAL / CAN_MODE_LOOPBACK */
uint8_t can_send(uint32_t id, const uint8_t *data, uint8_t len);
uint8_t can_recv(uint32_t *id, uint8_t *data, uint8_t *len, uint32_t timeout_ms);
int     can_selftest(void);                                       /* 0 = 全部通过，非 0 = 失败点编号 */

#endif /* __CAN_H */
