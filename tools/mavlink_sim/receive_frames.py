#!/usr/bin/env python3
"""
receive_frames.py —— RPi 侧临时 MAVLink v2 接收/解码打印工具。

用途：配合 send_frames.py 使用，在 M2（UART/MAVLink 收发帧层）真正的 C++
解码实现落地之前，先验证串口物理链路通、帧能收全、内容对得上。

这是临时验证工具，不是 cns_rpi 项目本身要交付的代码，不会被 CMake 构建，
等 M2 的 C++ 解码层写好后用真正的实现替代这里的验证方式。

依赖：pymavlink（用官方库解码帧，不手写 CRC/帧格式）。

字段字典依据：见 send_frames.py 顶部注释和 docs/V1设计文档.md §4.1/4.2。
"""

import argparse
import functools
import os
import struct

print = functools.partial(print, flush=True)  # 非交互式运行(如通过SSH管道)时stdout默认全缓冲，强制flush避免看不到输出

os.environ.setdefault("MAVLINK20", "1")  # 强制用 MAVLink v2，和 send_frames.py 保持一致

from pymavlink import mavutil  # noqa: E402

MODULE_NAMES = ["GNSS", "IMU", "BARO", "BATTERY", "LORA", "5G", "STORAGE", "REMOTE_ID",
                "DISPLAY", "CONTROL", "ALARM", "SYSTEM", "ESTIMATOR", "BUSINESS"]
STATE_NAMES = ["UNINITIALIZED", "STARTING", "ONLINE", "DEGRADED", "OFFLINE", "FAILED", "DISABLED"]
SEVERITY_NAMES = ["INFO", "WARNING", "ERROR", "CRITICAL", "FATAL"]

TUNNEL_PT_ALARM_TABLE = 0x8001
TUNNEL_PT_MESSAGE_LOG = 0x8002


def decode_modstat(name: str, value: int) -> str:
    part = 0 if name.rstrip("\x00") == "MODSTAT0" else 1
    start = 0 if part == 0 else 8
    packed = value & 0xFFFFFFFF
    parts = []
    for i in range(8):
        index = start + i
        if index >= len(MODULE_NAMES):
            break
        state = (packed >> (i * 4)) & 0x0F
        state_name = STATE_NAMES[state] if state < len(STATE_NAMES) else f"未知({state})"
        parts.append(f"{MODULE_NAMES[index]}={state_name}")
    return " ".join(parts)


def decode_bat2stat(value: int) -> str:
    packed = value & 0xFFFFFFFF
    voltage_mv = packed & 0xFFFF
    percent = (packed >> 16) & 0xFF
    low_voltage = (packed >> 24) & 0x01
    return f"电压={voltage_mv}mV 电量={percent}% 低电压标志={low_voltage}"


def decode_motorpwm(value: int) -> str:
    packed = value & 0xFFFFFFFF
    duties = [(packed >> (i * 8)) & 0xFF for i in range(4)]
    return f"占空比%={duties}"


def decode_gnss_sat(value: int) -> str:
    packed = value & 0xFFFFFFFF
    gps_visible = packed & 0xFF
    bds_visible = (packed >> 8) & 0xFF
    gps_used = (packed >> 16) & 0xFF
    bds_used = (packed >> 24) & 0xFF
    return f"GPS可见={gps_visible} 北斗可见={bds_visible} GPS使用={gps_used} 北斗使用={bds_used}"


def decode_envhum(value: int) -> str:
    return f"相对湿度={value / 10.0}%"


NAMED_VALUE_INT_DECODERS = {
    "MODSTAT0": decode_modstat,
    "MODSTAT1": decode_modstat,
    "BAT2STAT": lambda name, value: decode_bat2stat(value),
    "MOTORPWM": lambda name, value: decode_motorpwm(value),
    "GNSS_SAT": lambda name, value: decode_gnss_sat(value),
    "ENVHUM": lambda name, value: decode_envhum(value),
}


def decode_named_value_int(msg) -> str:
    name = msg.name.rstrip("\x00") if isinstance(msg.name, str) else msg.name.decode("ascii", "ignore").rstrip("\x00")
    decoder = NAMED_VALUE_INT_DECODERS.get(name)
    if decoder is None:
        return f"(未知 name={name}，不解读)"
    return decoder(name, msg.value)


def decode_tunnel(msg) -> str:
    payload = bytes(msg.payload[: msg.payload_length])
    if msg.payload_type == TUNNEL_PT_ALARM_TABLE:
        if len(payload) < 2:
            return "告警表：payload 太短"
        ver, active_count = struct.unpack_from("<BB", payload, 0)
        rows = []
        offset = 2
        while offset + 7 <= len(payload):
            source_id, fault_code, severity, active, age_s = struct.unpack_from("<BHBBH", payload, offset)
            severity_name = SEVERITY_NAMES[severity] if severity < len(SEVERITY_NAMES) else f"未知({severity})"
            rows.append(f"[source_id={source_id} fault_code={fault_code} severity={severity_name} "
                        f"active={active} age_s={age_s}]")
            offset += 7
        return f"告警表 ver={ver} active_count={active_count} rows={rows}"
    if msg.payload_type == TUNNEL_PT_MESSAGE_LOG:
        if len(payload) < 3:
            return "日志增量：payload 太短"
        latest_seq, count = struct.unpack_from("<HB", payload, 0)
        entries = []
        offset = 3
        while offset + 8 <= len(payload):
            sequence, message_id, hh, mm, ss, severity = struct.unpack_from("<HHBBBB", payload, offset)
            severity_name = SEVERITY_NAMES[severity] if severity < len(SEVERITY_NAMES) else f"未知({severity})"
            entries.append(f"[seq={sequence} message_id={message_id} time={hh:02d}:{mm:02d}:{ss:02d} "
                           f"severity={severity_name}]")
            offset += 8
        return f"日志增量 latest_seq={latest_seq} count={count} entries={entries}"
    return f"(未知 payload_type=0x{msg.payload_type:04X}，不解读)"


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
            msg_type = msg.get_type()
            line = (f"[{received}] sysid={msg.get_srcSystem()} compid={msg.get_srcComponent()} "
                    f"type={msg_type} -> {msg.to_dict()}")
            if msg_type == "NAMED_VALUE_INT":
                line += f"\n      解读: {decode_named_value_int(msg)}"
            elif msg_type == "TUNNEL":
                line += f"\n      解读: {decode_tunnel(msg)}"
            print(line)
    except KeyboardInterrupt:
        print(f"\n已停止，共收到 {received} 帧")


if __name__ == "__main__":
    main()
