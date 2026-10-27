#include "crc32.h"

/* 多项式取反形式（查表法标准写法） */
#define CRC32_POLY  0xEDB88320U

static uint32_t table[256];
static int      inited = 0;

/* 生成 CRC32 查表（只需一次） */
void crc32_init(void)
{
    if (inited) return;
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1U) ? (CRC32_POLY ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    inited = 1;
}

uint32_t crc32_start(void)
{
    return 0xFFFFFFFFU;
}

/* 流式累加：每收到一包数据就喂进来，不用把整包缓存在 RAM */
uint32_t crc32_step(uint32_t crc, const uint8_t *data, size_t len)
{
    if (!inited) crc32_init();
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    return crc;
}

uint32_t crc32_finish(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFU;
}

/* 一次性计算（适合小缓冲） */
uint32_t crc32_calc(const uint8_t *data, size_t len)
{
    return crc32_finish(crc32_step(crc32_start(), data, len));
}
