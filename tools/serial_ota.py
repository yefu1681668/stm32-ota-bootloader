#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
串口 OTA 上位机工具（配合 BootLoader 的 enter_upgrade 协议）
用法：
    python serial_ota.py COM3 app.bin
依赖：
    pip install pyserial
协议（与 boot/Core/Src/ota.c 保持一致）：
    1) 发 '\\r' 唤醒 Boot 的升级模式
    2) 发 "START:<size>:<crc32>\\r\\n"
    3) 等 "READY"
    4) 分帧发送：[offset 4B 大端][len 1B][data len]，每帧回 'A'
       —— 某帧没回 'A' 就重发该帧（帧自带 offset，可重发，安全）
    5) 结束帧：[0xFFFFFFFF][0x00]
    6) Boot 校验后写参数并软复位，自动把固件搬进 APP 区
说明：zlib.crc32 与单片机的 CRC32（多项式 0xEDB88320）完全一致，可直接互验。
v4：帧级自动重传（最多 10 次/帧），根治串口偶发丢字节导致的整包失败。
"""
import sys, struct, zlib, time, serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
BIN  = sys.argv[2] if len(sys.argv) > 2 else "app.bin"
BAUD = 115200
CHUNK = 128
MAX_RETRY = 3        # 整包尝试次数
FRAME_RETRY = 10     # 单帧最多重发次数

def drain(ser, quiet=0.25):
    """把接收缓冲读干净，直到连续 quiet 秒没有新数据"""
    old = ser.timeout
    ser.timeout = quiet
    total = 0
    while True:
        chunk = ser.read(64)
        if not chunk:
            break
        total += len(chunk)
    ser.timeout = old
    return total

def try_once(ser, size, crc, data):
    """完整跑一次升级流程，成功返回 True"""
    # 1) 唤醒升级模式：反复发 \r，最多等 10 秒
    #    （板子如果正在跑 APP，用户只要在这 10 秒内按一下复位键，
    #      Boot 的 2 秒窗口就会收到 \r，进入升级模式）
    print("提示：如果板子正在跑 APP，请在接下来 10 秒内按一下复位键……")
    entered = False
    end_t = time.time() + 10
    while time.time() < end_t:
        ser.write(b"\r")
        resp = ser.read_until(b"upgrade mode", 0.5)
        if b"upgrade mode" in resp:
            entered = True
            break
        drain(ser, 0.1)
    if not entered:
        print("（没看到升级模式提示，直接尝试握手——若板子已在升级模式也能成功）")

    # 2) 握手：START -> READY
    ser.write(("START:%d:%08X\r\n" % (size, crc)).encode())
    old_to = ser.timeout
    ser.timeout = 8
    resp = ser.read_until(b"READY")
    ser.timeout = old_to
    print("BOOT:", resp.decode(errors="ignore").strip())
    if b"READY" not in resp:
        print("ERR: 未收到 READY（板子没进升级模式）")
        return False
    time.sleep(0.05)
    ser.reset_input_buffer()

    # 3) 分帧发送（帧级重传）
    off = 0
    while off < size:
        n = min(CHUNK, size - off)
        frame = struct.pack(">IB", off, n) + data[off:off + n]
        sent = False
        for attempt in range(1, FRAME_RETRY + 1):
            ser.write(frame)
            ack = ser.read(1)
            if ack == b"A":
                sent = True
                break
            # 没收到 A：把 Boot 打的提示（RESEND 等）读干净，然后重发
            noise = drain(ser, 0.2)
            print(f"  帧 off={off} 第 {attempt} 次失败（收到 {ack!r}，清掉 {noise}B 提示），重发…")
            time.sleep(0.05)
        if not sent:
            print(f"ERR: 第 {off} 字节连续 {FRAME_RETRY} 次重发都失败")
            return False
        off += n
        print(f"已发 {off}/{size}")
        time.sleep(0.005)           # 5ms 喘息

    # 4) 结束帧
    time.sleep(0.02)
    ser.reset_input_buffer()
    ser.write(struct.pack(">IB", 0xFFFFFFFF, 0))

    # 5) 等 Boot 校验（注意：要匹配完整的 "VERIFIED OK"，只读到 VERIFIED 会误判失败）
    ser.timeout = 10
    out = ser.read_until(b"VERIFIED OK")
    ser.timeout = 3
    if b"VERIFIED OK" in out:
        print("BOOT:", out.decode(errors="ignore").strip())
        print("校验通过！等待 Boot 搬移固件并跳转 APP……")
        time.sleep(2.0)
        tail = ser.read(800).decode(errors="ignore")
        if tail.strip():
            print(tail.strip())
        merged = out.decode(errors="ignore") + tail
        if ("jump to APP" in merged) or ("running" in merged):
            return True
        print("WARN: 校验通过但没检测到 APP 启动（没看到 jump to APP / running），可能搬移或跳转失败。")
        return False

    out += ser.read(200)
    print("BOOT:", out.decode(errors="ignore").strip())
    return False

def main():
    with open(BIN, "rb") as f:
        data = f.read()
    size = len(data)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    print(f"固件: {BIN}  大小={size}  CRC32=0x{crc:08X}")

    ser = serial.Serial(PORT, BAUD, timeout=3)
    time.sleep(0.4)

    ok = False
    for attempt in range(1, MAX_RETRY + 1):
        print(f"\n===== 第 {attempt}/{MAX_RETRY} 次尝试 =====")
        if try_once(ser, size, crc, data):
            ok = True
            break
        if attempt < MAX_RETRY:
            print(">>> 失败！2 秒后自动重试（若板子完全无响应，可按一下复位键）")
            time.sleep(2)

    ser.close()
    if ok:
        print("\n===== 升级成功 =====")
        print("固件已校验并写入参数区，Boot 软复位后已搬进 APP 区并跳转。")
        print("看板子：PC13 板载 LED 应每 0.5 秒闪一次（APP 心跳）。")
        print("想看 APP 持续日志：关掉本脚本后用串口助手打开同端口即可。")
    else:
        print("\n三次都失败。检查：1) 板子是否上电 2) TX/RX 是否接反 3) 串口是否被其他程序占用")

if __name__ == "__main__":
    main()
