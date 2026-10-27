# STM32F103 串口 IAP + 双分区 OTA Bootloader

基于 **STM32F103C8T6** 的固件在线升级（OTA/IAP）完整实现：PC 通过串口推送固件 → Bootloader 接收并暂存到外部 SPI Flash（W25Q16）→ CRC32 整包校验 → 搬运到 APP 分区 → 七步安全跳转 → 新固件运行。

> 从零跟随尚硅谷《零基础 OTA-BootLoader 教程》学习并实践，在其基础上针对 C8T6（20KB RAM / 64KB Flash）做了适配，并自行加固了通信可靠性（帧级重传、UART 错误标志处理）。

## 功能特性

- ✅ **双分区设计**：Boot 16KB / APP 47KB / 参数区 1KB，互不干扰
- ✅ **外部 Flash 暂存**：固件先写入 W25Q16（SPI Mode0），校验通过才动内部 Flash，断电不变砖
- ✅ **CRC32 整包校验**：多项式 `0xEDB88320`，与 PC 端 zlib 结果一致，可互验
- ✅ **帧级重传**：每帧带绝对 offset，坏帧自动重发（幂等），借鉴 YMODEM 思想
- ✅ **UART 错误标志自愈**：轮询接收前清 ORE/NE/FE/PE，杜绝"传一半突然静默"
- ✅ **七步安全跳转**：关中断 → 清 NVIC → 时钟复位 → HAL DeInit → 停 SysTick → 设 MSP → 跳 PC
- ✅ **APP 侧自检**：`SCB->VTOR` 重定位 + `__enable_irq()`，双保险

## Flash 分区

| 分区 | 起始地址 | 大小 | 用途 |
|---|---|---|---|
| Boot | `0x08000000` | 16KB (`0x4000`) | Bootloader，含升级协议与搬移逻辑 |
| APP | `0x08004000` | 47KB (`0xBC00`) | 应用程序 |
| 参数区 | `0x0800FC00` | 1KB | OTA 标记（magic/state/size/crc） |

## 升级流程

```
PC (serial_ota.py)                         板子 (Boot)
      │  \r 唤醒(10s轮询)                       │
      │ ─────────────────────────────────────▶ │ 2s 窗口内收到任意键 → 升级模式
      │  START:<size>:<crc32>\r\n               │
      │ ─────────────────────────────────────▶ │ 擦 W25Q 暂存区
      │ ◀───────────────────────────────────── │ READY
      │  [offset 4B][len 1B][data ≤128B] ×N     │
      │ ─────────────────────────────────────▶ │ 写 W25Q，每帧回 'A'（坏帧要求重发）
      │  结束帧 [FF FF FF FF][00]               │
      │ ─────────────────────────────────────▶ │ 读回整包重算 CRC32
      │ ◀───────────────────────────────────── │ VERIFIED OK → 写参数区 → 软复位
      │                                         │ 复位后：检测 PENDING → 搬 APP 区 → 跳转
```

## 目录结构

```
first/
├── boot/               # Bootloader 工程（Keil MDK，Target: boot）
│   └── Core/Src/       # main.c / ota.c / flash_if.c / crc32.c / w25qxx.c
├── app/                # 应用工程（Keil MDK，Target: app，IROM1=0x08004000）
│   └── Core/Src/main.c # LED 心跳 + 串口打印 + VTOR 重定位
├── tools/
│   └── serial_ota.py   # PC 端升级工具（Python + pyserial，含自动重试）
└── docs/
    └── TROUBLESHOOTING.md  # 21 个真实踩坑记录与排障方法论
```

## 快速开始

### 1. 硬件

- STM32F103C8T6 最小系统板（板载 LED 在 PC13）
- W25Q16 SPI Flash（CS=PA4，SCK=PA5，MISO=PA6，MOSI=PA7）
- CH340 USB 转串口（TX→PA10，RX→PA9，共地）
- ST-Link（仅首次烧 Boot 需要）

### 2. 编译烧录 Boot

1. Keil 打开 `boot/MDK-ARM/boot.uvprojx`，F7 编译（Code≈11KB < 16KB 护栏）
2. ST-Link 下载（**Erase Sectors**，勿 Full Chip）

### 3. 编译 APP 并生成 bin

1. Keil 打开 `app/MDK-ARM/app.uvprojx`（Target 为 **app**）
2. AfterMake 已配置 `fromelf --bin`，编译后在 `MDK-ARM/app/` 生成 `app.bin`

### 4. 串口升级（不再需要 ST-Link）

```bash
pip install pyserial
python tools/serial_ota.py COM16 app/MDK-ARM/app/app.bin
```

脚本会在 10 秒窗口内自动轮询唤醒，看到 `VERIFIED OK` → `[APP] running!` 即升级成功，PC13 LED 以 0.5s 间隔闪烁。

## 串口日志示例

```
[BOOT] OTA Bootloader v1.0
CRC32 self-test: CBF43926 (OK)
W25Q ID = EF4014
[BOOT] press any key in 2s to upgrade, else run APP
[BOOT] pending firmware found, size=5136
[BOOT] jump to APP @0x08004000
[APP] running! jump from Boot OK
[APP] VTOR = 0x08004000
[APP] alive, tick=500
```

## Roadmap

- [x] 阶段一：串口 IAP + W25Q 暂存 + 双分区跳转（当前）
- [ ] 阶段二：CAN 双板互通（SN65HVD230，500kbps，传输抽象层替换）
- [ ] 阶段三：LoRa 远程升级（E22-470T22S，470MHz UART 透传）
- [ ] boot_try 失败保护：APP 连续 3 次未确认则停在 Boot 等重升

## 已知现象

跳转瞬间串口可能出现 `jump to APP @//8.0.64.0/` 之类的打印毛刺——这是中断关闭与外设状态切换瞬间的字符错乱，不影响功能。

## License

[MIT](LICENSE)
