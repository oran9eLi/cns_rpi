#!/usr/bin/env python3
"""
send_frames.py —— 开发机侧 MAVLink v2 帧模拟发送器。

用途：在 M2（UART/MAVLink 收发帧层）真正实现前，用这个脚本在开发机上模拟
STM32 通过串口向 RPi 发送数据，验证串口物理链路和帧内容，不依赖真机 STM32。

只在开发/测试时用，不是 cns_rpi 项目本身要交付的代码，不会被 CMake 构建。

依赖：pymavlink（用官方库编码帧，不手写 CRC/帧格式）。

字段字典依据：固件源码 Framework/Src/px4lite_mavlink_tx.c、
Framework/Inc/px4lite_remote_tunnel.h、Framework/Inc/px4lite_types.h
（不是猜的，逐字段核对过），详见 docs/V1设计文档.md §4.1/4.2。
"""

import argparse
import functools
import os
import struct
import time

print = functools.partial(print, flush=True)  # 非交互式运行(如通过SSH管道)时stdout默认全缓冲，强制flush避免看不到输出

os.environ.setdefault("MAVLINK20", "1")  # 强制用 MAVLink v2，不用v1（项目约定见 docs/V1设计文档.md §4）

from pymavlink import mavutil  # noqa: E402  (必须在设置 MAVLINK20 环境变量之后导入)

# Px4Lite_State_t（docs/V1设计文档.md §4.1）
STATE_ONLINE = 2
STATE_DEGRADED = 3

# Px4Lite_AlarmSeverity_t（docs/V1设计文档.md §4.2）
SEVERITY_WARNING = 1

TUNNEL_PT_ALARM_TABLE = 0x8001
TUNNEL_PT_MESSAGE_LOG = 0x8002


def build_connection(port: str, baud: int, source_system: int):
    return mavutil.mavlink_connection(
        port,
        baud=baud,
        source_system=source_system,
        dialect="common",
    )


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


def _named_value_int(conn, now_ms: int, name: str, value: int):
    conn.mav.named_value_int_send(
        time_boot_ms=now_ms,
        name=name.encode("ascii"),  # name 长度不足8字节时pymavlink会自动补0，超过会截断
        value=value,
    )


def send_modstat(conn, now_ms: int, part: int, module_states: list[int]):
    """MODSTAT0(part=0，模块0-7)/MODSTAT1(part=1，模块8-13)。每模块4bit，LSB在前。"""
    start = 0 if part == 0 else 8
    packed = 0
    for i in range(8):
        index = start + i
        if index < len(module_states):
            packed |= (module_states[index] & 0x0F) << (i * 4)
    _named_value_int(conn, now_ms, "MODSTAT0" if part == 0 else "MODSTAT1", packed)


def send_bat2stat(conn, now_ms: int, voltage_mv: int, percent: int, low_voltage: bool):
    packed = (voltage_mv & 0xFFFF) | ((percent & 0xFF) << 16) | ((1 if low_voltage else 0) << 24)
    _named_value_int(conn, now_ms, "BAT2STAT", packed)


def send_motorpwm(conn, now_ms: int, duty_percent: list[int]):
    """最多4个电机，每个1字节占空比。"""
    packed = 0
    for i, duty in enumerate(duty_percent[:4]):
        packed |= (duty & 0xFF) << (i * 8)
    _named_value_int(conn, now_ms, "MOTORPWM", packed)


def send_gnss_sat(conn, now_ms: int, gps_visible: int, bds_visible: int, gps_used: int, bds_used: int):
    packed = (gps_visible & 0xFF) | ((bds_visible & 0xFF) << 8) | ((gps_used & 0xFF) << 16) | ((bds_used & 0xFF) << 24)
    _named_value_int(conn, now_ms, "GNSS_SAT", packed)


def send_envhum(conn, now_ms: int, humidity_pct_x10: int):
    """humidity_pct_x10=535 表示 53.5% 相对湿度。"""
    _named_value_int(conn, now_ms, "ENVHUM", humidity_pct_x10)


def _tunnel(conn, payload_type: int, payload: bytes):
    # MAVLink TUNNEL payload 固定 128 字节，不足的部分补 0。
    padded = payload + b"\x00" * (128 - len(payload))
    conn.mav.tunnel_send(
        target_system=0,
        target_component=0,
        payload_type=payload_type,
        payload_length=len(payload),
        payload=padded,
    )


def send_tunnel_alarm_table(conn, records: list[tuple]):
    """records: [(source_id, fault_code, severity, active, age_s), ...]，最多14行。"""
    ver = 1
    active_count = sum(1 for r in records if r[3])
    payload = struct.pack("<BB", ver, active_count)
    for source_id, fault_code, severity, active, age_s in records[:14]:
        payload += struct.pack("<BHBBH", source_id, fault_code, severity, 1 if active else 0, age_s)
    _tunnel(conn, TUNNEL_PT_ALARM_TABLE, payload)


def send_tunnel_message_log(conn, latest_seq: int, entries: list[tuple]):
    """entries: [(sequence, message_id, hh, mm, ss, severity), ...]，最多9条；entries为空即心跳。"""
    payload = struct.pack("<HB", latest_seq, len(entries))
    for sequence, message_id, hh, mm, ss, severity in entries[:9]:
        payload += struct.pack("<HHBBBB", sequence, message_id, hh, mm, ss, severity)
    _tunnel(conn, TUNNEL_PT_MESSAGE_LOG, payload)


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

    # 14 个模块（Px4Lite_ModuleId_t 顺序），演示一个 DEGRADED，其余 ONLINE
    module_states = [STATE_ONLINE] * 14
    module_states[3] = STATE_DEGRADED  # BATTERY 模块降级，作为示例

    print(f"开始向 {args.port} @ {args.baud} baud 发送模拟 MAVLink v2 帧（system_id={args.sysid}），Ctrl+C 停止")

    sent_rounds = 0
    start = time.monotonic()
    try:
        while True:
            now_ms = int((time.monotonic() - start) * 1000)

            # 每条消息之间留一点间隔，避免在低波特率下一次性突发发送把 CH340 缓冲冲爆
            # （实测：12条消息毫无间隔连续发出去，会在57600波特率下导致丢字节/帧错位）。
            msg_gap_s = 0.02
            send_heartbeat(conn); time.sleep(msg_gap_s)
            send_gps_raw_int(conn, now_ms); time.sleep(msg_gap_s)
            send_attitude(conn, now_ms); time.sleep(msg_gap_s)
            send_sys_status(conn); time.sleep(msg_gap_s)
            send_modstat(conn, now_ms, 0, module_states); time.sleep(msg_gap_s)
            send_modstat(conn, now_ms, 1, module_states); time.sleep(msg_gap_s)
            send_bat2stat(conn, now_ms, voltage_mv=11800, percent=75, low_voltage=False); time.sleep(msg_gap_s)
            send_motorpwm(conn, now_ms, duty_percent=[50, 50, 48, 52]); time.sleep(msg_gap_s)
            send_gnss_sat(conn, now_ms, gps_visible=8, bds_visible=6, gps_used=6, bds_used=4); time.sleep(msg_gap_s)
            send_envhum(conn, now_ms, humidity_pct_x10=535); time.sleep(msg_gap_s)
            send_tunnel_alarm_table(conn, records=[(3, 101, SEVERITY_WARNING, True, 12)]); time.sleep(msg_gap_s)  # source_id=3(BATTERY)
            send_tunnel_message_log(conn, latest_seq=sent_rounds, entries=[])  # 只发心跳，不带日志条目

            sent_rounds += 1
            print(f"[{sent_rounds}] 已发送一轮：HEARTBEAT/GPS_RAW_INT/ATTITUDE/SYS_STATUS/"
                  f"MODSTAT0/MODSTAT1/BAT2STAT/MOTORPWM/GNSS_SAT/ENVHUM/TUNNEL(告警表+日志心跳)")

            if args.count and sent_rounds >= args.count:
                break
            time.sleep(interval)
    except KeyboardInterrupt:
        print("\n已停止")


if __name__ == "__main__":
    main()
