#!/usr/bin/env python3
"""
receive_frames.py —— RPi 侧临时 MAVLink v2 接收/解码打印工具。

用途：配合 send_frames.py 使用，在 M2（UART/MAVLink 收发帧层）真正的 C++
解码实现落地之前，先验证串口物理链路通、帧能收全、内容对得上。

这是临时验证工具，不是 cns_rpi 项目本身要交付的代码，不会被 CMake 构建，
等 M2 的 C++ 解码层写好后用真正的实现替代这里的验证方式。

依赖：pymavlink（用官方库解码帧，不手写 CRC/帧格式）。
"""

import argparse
import functools
import os

print = functools.partial(print, flush=True)  # 非交互式运行(如通过SSH管道)时stdout默认全缓冲，强制flush避免看不到输出

os.environ.setdefault("MAVLINK20", "1")  # 强制用 MAVLink v2，和 send_frames.py 保持一致

from pymavlink import mavutil  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description="接收并解码打印 MAVLink v2 帧")
    parser.add_argument("--port", required=True, help="串口设备路径，例如 /dev/ttyUSB0 或 /dev/serial0")
    parser.add_argument("--baud", type=int, default=57600, help="波特率，需要和发送端一致")
    args = parser.parse_args()

    conn = mavutil.mavlink_connection(args.port, baud=args.baud, dialect="common")

    print(f"监听 {args.port} @ {args.baud} baud，等待 MAVLink v2 帧（Ctrl+C 停止）...")

    received = 0
    try:
        while True:
            msg = conn.recv_match(blocking=True, timeout=5)
            if msg is None:
                print("  ...5秒内没收到帧")
                continue
            received += 1
            print(f"[{received}] sysid={msg.get_srcSystem()} compid={msg.get_srcComponent()} "
                  f"type={msg.get_type()} -> {msg.to_dict()}")
    except KeyboardInterrupt:
        print(f"\n已停止，共收到 {received} 帧")


if __name__ == "__main__":
    main()
