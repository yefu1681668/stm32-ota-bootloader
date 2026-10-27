# 排障记录（TROUBLESHOOTING）

> 本项目从零到真机跑通共踩了 **21 个坑**，这里按病根分类记录。完整图文版见工作区《阶段一故障复盘手册.html》。

## 病根总览

| 类别 | 数量 | 一句话 |
|---|---|---|
| A. Keil 工程配置 | 7 | "文件夹里有文件" ≠ "工程会编译它"；复制工程后配置残留是重灾区 |
| B. 编译与代码位置 | 3 | 被宏包住的代码不参与编译；ARMCC V5 按 GBK 读 UTF-8 会乱码 |
| C. 环境工具链 | 2 | 一个串口只能被一个程序占用 |
| D. 通信协议健壮性 | 6 | HAL 轮询接收不处理错误标志；协议必须能自愈 |
| E. 运行时与架构 | 3 | 中断状态会跨"程序"传递；串口判断别读半截就下结论 |

## 三个最致命的坑（附修法）

### 1. HAL_UART_Receive 不检查 ORE → 传一半突然静默

本项目所用 HAL 版本的轮询接收（`stm32f1xx_hal_uart.c` 1224~1298 行）只处理超时/忙，
**完全不处理 ORE/NE/FE/PE**。一次 USB 串口抖动造成溢出后，错误标志一直挂着，
之后接收要么永久失败、要么静默丢字节导致后续全体错位。

**修法**：
```c
static int uart_getc(uint8_t *c, uint32_t timeout)
{
    uint32_t err = huart1.Instance->SR;
    if (err & (UART_FLAG_ORE | UART_FLAG_NE | UART_FLAG_FE | UART_FLAG_PE))
    {
        /* 读 DR 清掉错误标志 */
        (void)huart1.Instance->DR;
        huart1.RxState = HAL_UART_STATE_READY;
    }
    return (HAL_UART_Receive(&huart1, c, 1, timeout) == HAL_OK) ? 1 : 0;
}
```
再加 `uart_drain()`：出错后一直读到线路安静 100ms，把残帧全扔掉。

### 2. 三次都精确死在第 1024 字节 → 帧错位

丢一个字节后，Boot 把数据当帧头解析，算出荒唐的 offset/len。而协议对帧头零校验。
**修法**：帧头合法性校验（`offset < size` 且 `1 <= len <= 128`）+ 坏帧打印
`err#n hdr=XX XX XX XX XX` 诊断 + **帧级重传**（每帧带绝对 offset，重发=覆盖写同一地址，幂等）。
"每次都死在同一位置" = 确定性问题，不是抖动。

### 3. APP 跳过去就"死"（LED 不闪、串口静默）

七步跳转第①步 `__disable_irq()` 关了全局中断，跳进 APP 后中断仍是关的；
`HAL_Delay()` 依赖 SysTick 中断喂 `uwTick` → uwTick 永远不变 → 死循环在第一句 printf 之前。

**修法**：APP 入口处 `SCB->VTOR = 0x08004000;` 之后立刻 `__enable_irq();`。

## 高频小坑速查

- **Add Files 报 "already a member" 但文件没进工程**：Keil 只编译 `.uvprojx` 里登记的文件；UI 抽风时直接编辑 XML 加 `<File>` 节点最可靠
- **`0x8004000` vs `0x08004000`**：同一个数（Keil 省略前导零）；但少一个尾 0（`0x0800400`）就是错地址
- **`VECT_TAB_OFFSET` 改了没反应**：整段被 `#if defined(USER_VECT_TAB_ADDRESS)` 包着，宏被注释 = 死代码
- **复制 boot→app 后三处必查**：IROM1（是否还指 0x08000000）、TargetName/OutputDirectory（是否还叫 boot）、main.c（是否被 Boot 版覆盖）
- **脚本误报失败**：`read_until(b"VERIFIED")` 只读到前缀就判断，要读完整消息再下结论
- **JEDEC ID `EF 40 14`**：W25Q16 = 2MB（第 3 字节是容量密度码），驱动按 ID 自动识别，不写死容量
- **printf 无输出**：`fputc` 可能被 CubeMX 塞进 `#ifdef USE_FULL_ASSERT` 块（默认不编译）
- **中文乱码 #870-D**：ARMCC V5 按 GBK 读 UTF-8；固件日志尽量用英文

## 排障 4 步法

1. **看现象找确定性**：每次死在同一位置 → 确定性问题，找共性；偶发 → 才考虑接触/抖动
2. **确认改动真的生效**：有没有被编译进去？（`#if` 包住？工程登记了？烧的是新固件？）
3. **读源码别猜**：库函数的文档就是它的源码
4. **给协议留诊断口**：坏帧时打印原始字节，有诊断信息才能一眼定位

## 下次开工自查清单

- [ ] Boot IROM1 = `0x8000000/0x4000`；APP IROM1 = `0x8004000/0xBC00`
- [ ] APP 启动三件套：`SCB->VTOR=0x08004000` + `__enable_irq()` + `USER_VECT_TAB_ADDRESS` 宏已启用
- [ ] APP 工程 `TargetName=app`，AfterMake 有 `fromelf --bin`
- [ ] 跑脚本前关掉串口助手（反之亦然）
- [ ] 板子跑 APP 时，脚本启动后 10 秒内按一次复位
