#include "flash_if.h"
#include "stm32f1xx_hal.h"

/* ---------- 解锁（擦/写前必须调用）---------- */
int FLASH_If_Init(void)
{
    HAL_FLASH_Unlock();
    return FLASH_IF_OK;
}

/* ---------- 擦除：把 [start,end) 覆盖到的整页都擦掉 ---------- */
int FLASH_If_Erase(uint32_t start, uint32_t end)
{
    if (start < FLASH_BASE_ADDR ||
        end   >  FLASH_BASE_ADDR + FLASH_TOTAL_PAGES * FLASH_PAGE_SIZE)
        return FLASH_IF_ERR_ERASE;

    FLASH_EraseInitTypeDef ei = {0};
    uint32_t page_error = 0;

    ei.TypeErase   = FLASH_TYPEERASE_PAGES;
    ei.PageAddress = start;
    ei.NbPages     = (end - start + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;

    HAL_FLASH_Unlock();
    if (HAL_FLASHEx_Erase(&ei, &page_error) != HAL_OK)
        return FLASH_IF_ERR_ERASE;
    return FLASH_IF_OK;
}

/* ---------- 写：F1 只能“半字(16bit)”编程，地址/长度必须 2 对齐 ---------- */
int FLASH_If_Write(uint32_t dst, const uint8_t *src, uint32_t len)
{
    if ((dst & 1U) || (len & 1U))
        return FLASH_IF_ERR_ALIGN;

    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < len; i += 2)
    {
        uint16_t half = (uint16_t)(src[i] | (src[i + 1] << 8));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, dst + i, half) != HAL_OK)
            return FLASH_IF_ERR_WRITE;
    }
    return FLASH_IF_OK;
}

/* ---------- 回读比对 ---------- */
int FLASH_If_Verify(uint32_t dst, const uint8_t *src, uint32_t len)
{
    const volatile uint8_t *p = (const volatile uint8_t *)dst;
    for (uint32_t i = 0; i < len; i++)
        if (p[i] != src[i])
            return 1;   /* 不一致 */
    return 0;           /* 一致 */
}

void FLASH_If_Lock(void)
{
    HAL_FLASH_Lock();
}
