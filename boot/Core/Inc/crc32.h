#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>
#include <stddef.h>

/* CRC32（ZIP 标准，多项式 0xEDB88320，初值/结果均异或 0xFFFFFFFF）
   课程约定：固件整包用 CRC32 校验，防止传输/存储比特翻转 */

void     crc32_init(void);                       /* 第一次使用前调用：生成 256 项查表 */
uint32_t crc32_start(void);                      /* 初始值 0xFFFFFFFF */
uint32_t crc32_step(uint32_t crc,
                    const uint8_t *data,
                    size_t len);                  /* 分块累加（流式） */
uint32_t crc32_finish(uint32_t crc);             /* 收尾异或 0xFFFFFFFF */
uint32_t crc32_calc(const uint8_t *data,
                    size_t len);                  /* 一次性计算（= start+step+finish） */

#endif /* CRC32_H */
