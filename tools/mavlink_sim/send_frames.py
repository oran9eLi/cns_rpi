#!/usr/bin/env python3
"""
send_frames.py —— 开发机侧 MAVLink v2 帧模拟发送器。

用途：在 M2（UART/MAVLink 收发帧层）真正实现前，用这个脚本在开发机上模拟
STM32 通过串口向 RPi 发送数据，验证串口物理链路和帧内容，不依赖真机 STM32。

只在开发/测试时用，不是 cns_rpi 项目本身要交付的代码，不会被 CMake 构建。

依赖：pymavlink（用官方库编码帧，不手写 CRC/帧格式）。
"""

import argparse
import functools
import os
import time

print = functools.partial(print, flush=True)  # 非交互式运行(如通过SSH管道)时stdout默认全缓冲，强制flush避免看不到输出

os.environ.setdefault("MAVLINK20", "1")  # 强制用 MAVLink v2，不用v1（项目约定见 docs/V1设计文档.md §4）

from pymavlink import mavutil  # noqa: E402  (必须在设置 MAVLINK20 环境变量之后导入)


def build_connection(port: str, baud: int, source_system: int):
    conn = mavutil.mavlink_connection(
        port,
        baud=baud,
        source_system=source_system,
        dialect="common",
    )
    return conn


def send_heartbeat(conn):
    conn.mav.heartbeat_send(
        type=mavutil.mavlink.MAV_TYPE_GENERIC,
        autopilot=mavutil.mavlink.MAV_AUTOPILOT_INVALID,
        base_mode=0,
        custom_mode=0,
        system_status=mavutil.mavlink.MAV_STATE_ACTIVE,
    )


def send_gps_raw_int(conn, now_ms: int):
    conn.mav.gps_raw_int_send(
        time_usec=now_ms * 1000,
        fix_type=mavutil.mavlink.GPS_FIX_TYPE_3D_FIX,
        lat=int(31.2304 * 1e7),   # 示例坐标：上海
        lon=int(121.4737 * 1e7),
        alt=50_000,               # mm
        eph=100,
        epv=100,
        vel=500,                  # cm/s
        cog=9000,                 # 0.01 度
        satellites_visible=10,
    )


def send_attitude(conn, now_ms: int):
    conn.mav.attitude_send(
        time_boot_ms=now_ms,
        roll=0.01,
        pitch=-0.02,
        yaw=1.57,
        rollspeed=0.0,
        pitchspeed=0.0,
        yawspeed=0.0,
    )


def send_sys_status(conn):
    conn.mav.sys_status_send(
        onboard_control_sensors_present=0,
        onboard_control_sensors_enabled=0,
        onboard_control_sensors_health=0,
        load=300,                  # 0.1%
        voltage_battery=12000,     # mV
        current_battery=1500,      # 0.01A
        battery_remaining=80,      # %
        drop_rate_comm=0,
        errors_comm=0,
        errors_count1=0,
        errors_count2=0,
        errors_count3=0,
        errors_count4=0,
    )


def send_named_value_int(conn, now_ms: int, name: str, value: int):
    # REMOTE_STATUS / REMOTE_MOTOR 等自定义业务数据借用 NAMED_VALUE_INT，
    # 靠 name 字段区分语义（见 docs/V1设计文档.md §4）。name 最长 10 字节。
    conn.mav.named_value_int_send(
        time_boot_ms=now_ms,
        name=name.encode("ascii")[:10],
        value=value,
    )


def main():
    parser = argparse.ArgumentParser(description="模拟 STM32 通过串口发送 MAVLink v2 帧")
    parser.add_argument("--port", required=True, help="串口设备路径，例如 /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=57600, help="波特率，默认 57600（最终以固件侧实际配置为准）")
    parser.add_argument("--sysid", type=int, default=1, help="MAVLink system_id，默认 1（对应 DCDW-001，主机）")
    parser.add_argument("--rate-hz", type=float, default=1.0, help="发送频率，默认 1Hz")
    parser.add_argument("--count", type=int, default=0, help="发送多少轮消息后退出，0 表示一直发（Ctrl+C 停止）")
    args = parser.parse_args()

    conn = build_connection(args.port, args.baud, args.sysid)
    interval = 1.0 / args.rate_hz

    print(f"开始向 {args.port} @ {args.baud} baud 发送模拟 MAVLink v2 帧（system_id={args.sysid}），Ctrl+C 停止")

    sent_rounds = 0
    start = time.monotonic()
    try:
        while True:
            now_ms = int((time.monotonic() - start) * 1000)

            send_heartbeat(conn)
            send_gps_raw_int(conn, now_ms)
            send_attitude(conn, now_ms)
            send_sys_status(conn)
            send_named_value_int(conn, now_ms, "REMOTE_STATUS", 1)

            sent_rounds += 1
            print(f"[{sent_rounds}] 已发送一轮：HEARTBEAT/GPS_RAW_INT/ATTITUDE/SYS_STATUS/NAMED_VALUE_INT")

            if args.count and sent_rounds >= args.count:
                break
            time.sleep(interval)
    except KeyboardInterrupt:
        print("\n已停止")


if __name__ == "__main__":
    main()
