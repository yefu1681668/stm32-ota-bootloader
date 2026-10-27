/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : APP 程序（被 BootLoader 通过串口 OTA 升级进来的固件）
  ******************************************************************************
  * @attention
  * 分区：Boot 0x08000000(16KB) | APP 0x08004000(0xBC00=47KB) | 参数区 0x0800FC00
  * 本工程 IROM1 = 0x08004000, Size = 0xBC00
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "ota_param.h"   /* 参数区结构（与 Boot 共用）：APP 自确认要回写 state/try_count */
#include "flash_if.h"    /* 内部 Flash 擦写：把“APP 已存活”写回参数区 */
/* USER CODE END Includes */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart1;

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);

/* USER CODE BEGIN 0 */
void ota_app_confirm(void);   /* 前向声明：函数体在文件末尾 USER CODE BEGIN 4 处 */
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
  /* 关键一步：把中断向量表指到 APP 区 0x08004000。
     芯片复位后 VTOR 默认指向 0x08000000（Boot 区），
     如果不重定位，APP 里一旦触发中断就会跳进 Boot 的向量表，直接跑飞。
     (system_stm32f1xx.c 里已配置 VECT_TAB_OFFSET=0x4000，这里是双保险) */
  SCB->VTOR = 0x08004000U;

  /* 关键第二步：Boot 的七步跳转法第①步是 __disable_irq()（关全局中断），
     跳进 APP 后中断仍然是关着的！HAL_Delay 依赖 SysTick 中断累加时钟，
     中断不开的话 HAL_Delay 永远等不到 tick -> APP 卡死在第一行 Delay。
     所以 APP 一进来必须自己把中断打开。 */
  __enable_irq();
  /* USER CODE END 1 */

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* Configure the system clock */
  SystemClock_Config();

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();

  /* USER CODE BEGIN 2 */
  HAL_Delay(100);                                /* 等串口电平稳定 */
  printf("[APP] running! jump from Boot OK\r\n");
  printf("[APP] VTOR = 0x%08lX\r\n", (unsigned long)SCB->VTOR);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint8_t confirmed = 0;                         /* 自确认只做一次 */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);      /* PC13 LED 翻转（核心板低电平点亮） */
    printf("[APP] alive, tick=%lu\r\n", (unsigned long)HAL_GetTick());

    /* 跑稳 2 秒后再自确认：此时中断/SysTick/GPIO 都正常 -> 证明 APP 真的活了 */
    if (!confirmed && HAL_GetTick() > 2000)
    {
        ota_app_confirm();
        confirmed = 1;
        printf("[APP] confirmed alive -> state=APP_OK\r\n");
    }

    HAL_Delay(500);                              /* 500ms 闪一次 = 心跳 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration (HSE 8MHz -> PLL x9 = 72MHz)
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief GPIO Init: PC13 输出（LED）
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();                  /* C8T6 板载 LED 在 PC13，先开 C 口时钟 */

  /* 默认输出高 = LED 灭（PC13 低电平点亮） */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);

  GPIO_InitStruct.Pin = GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
}

/**
  * @brief USART1 Init: 115200 8N1，与 Boot 同一套串口
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/* printf 重定向到 USART1 */
int fputc(int ch, FILE *f)
{
    (void)f;
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 0xFFFF);
    return ch;
}

/* ---------- APP 自确认：回写“我已存活” ----------
   Boot 跳转 APP 前会累加 try_count；只有当 APP 真正跑起来（中断开、SysTick 走、
   GPIO 可控）并主动回写 state=APP_OK + try_count=0，Boot 才认为这次升级成功。
   若 APP 起不来（卡死/跑飞），不会执行这函数 -> 下次 Boot 累加 try_count，
   连续 3 次后停 Boot 进升级模式等重刷。这就是单分区下的“失败保护”。 */
void ota_app_confirm(void)
{
    ota_param_t p;
    memcpy(&p, (const void *)PARAM_START, sizeof(ota_param_t));
    if (p.magic != OTA_MAGIC) return;          /* 没有 Boot 写的参数区，跳过 */
    if (p.state == OTA_STATE_APP_OK) return;   /* 已经确认过，不重复擦写 Flash */

    p.state = OTA_STATE_APP_OK;
    p.try_count = 0;
    FLASH_If_Init();
    FLASH_If_Erase(PARAM_START, PARAM_START + sizeof(ota_param_t));
    FLASH_If_Write(PARAM_START, (const uint8_t *)&p, sizeof(ota_param_t));
    FLASH_If_Lock();
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
