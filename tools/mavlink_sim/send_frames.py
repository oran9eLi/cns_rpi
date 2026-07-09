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

2026-07-07 跟固件 Formal_Framework PR#1 同步：`ENVHUM`→`HUMIDITY`、单条
`MOTORPWM`→`MOTOR12`/`MOTOR34` 双帧，新增 `LORASTAT`/`RIDSTAT`（RPi 专属）
和 `OPEN_DRONE_ID_*` 身份帧（M3c），字段布局详见 docs/固件对接-数据格式.md。

2026-07-09 跟 M4 官方通道切换同步：气压/温度改发官方 `SCALED_PRESSURE`，
电池2 不再用自定义 `BAT2STAT`，改发官方 `BATTERY_STATUS(id=1)`（跟电池1同一
消息、`id` 区分），详见 docs/superpowers/specs/2026-07-07-m4-json-payload-design.md。
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

# 演示用厂商唯一产品识别码（DCDWCNS1 + 12 字符 SN，GB/T 41300 字符集，不代表真实设备）
DEMO_VENDOR_ID = b"DCDWCNS1ABCDEFGH1234"
# id_or_mac[20]：只用于转发别的无人机身份数据，自己广播自己的身份时不适用，填全零
ID_OR_MAC_UNUSED = b"\x00" * 20


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


def send_battery_status(conn, battery_id: int, cell_mv: list[int], current_ca: int,
                         current_consumed_mah: int, energy_consumed_hj: int, percent: int):
    """官方 BATTERY_STATUS，用 id 区分电池1(id=0)/电池2(id=1)，字段语义/单位见
    mavlink_msg_battery_status.h 官方注释：voltages[10]槽位不用填 UINT16_MAX，
    voltages_ext[4]槽位不用填0（跟voltages不同，这里两块电池都只有普通cell、不
    用扩展槽位，所以voltages_ext全传0）。cell_mv 只传1个值——电池1/电池2各自
    能独立采集(两条BATTERY_STATUS、id=0/id=1分别对应)，但每一块电池内部只有
    一路整包电压采集，没有逐节电芯监测能力，之前这里演示过3/4个假cell值
    是编的，没有实际依据，2026-07-09订正。"""
    voltages = list(cell_mv) + [0xFFFF] * (10 - len(cell_mv))
    conn.mav.battery_status_send(
        id=battery_id,
        battery_function=mavutil.mavlink.MAV_BATTERY_FUNCTION_ALL,
        type=mavutil.mavlink.MAV_BATTERY_TYPE_LIPO,
        temperature=2500,          # 0.01 degC，演示值 25.00度
        voltages=voltages,
        current_battery=current_ca,        # 0.01A
        current_consumed=current_consumed_mah,  # mAh
        energy_consumed=energy_consumed_hj,     # hJ (0.01Wh)
        battery_remaining=percent,
        voltages_ext=[0, 0, 0, 0],
    )


def send_scaled_pressure(conn, now_ms: int, press_abs_hpa: float, temperature_cdeg: int):
    """官方 SCALED_PRESSURE，替代此前自定义的 BAROTEMP/BAROPRES。"""
    conn.mav.scaled_pressure_send(
        time_boot_ms=now_ms,
        press_abs=press_abs_hpa,
        press_diff=0.0,
        temperature=temperature_cdeg,
    )


def send_motor12(conn, now_ms: int, duty1: int, duty2: int, run_state: bool, speed_level: int):
    """电机1/2占空比，替代原单条MOTORPWM。run_state/speed_level跟MOTOR34帧携带同一份
    整机状态的冗余拷贝，两帧值应保持一致。"""
    packed = ((duty1 & 0xFF) | ((duty2 & 0xFF) << 8) | ((1 if run_state else 0) << 16) |
              ((speed_level & 0xFF) << 24))
    _named_value_int(conn, now_ms, "MOTOR12", packed)


def send_motor34(conn, now_ms: int, duty3: int, duty4: int, run_state: bool, speed_level: int):
    """电机3/4占空比，布局同 send_motor12。"""
    packed = ((duty3 & 0xFF) | ((duty4 & 0xFF) << 8) | ((1 if run_state else 0) << 16) |
              ((speed_level & 0xFF) << 24))
    _named_value_int(conn, now_ms, "MOTOR34", packed)


def send_gnss_sat(conn, now_ms: int, gps_visible: int, bds_visible: int, gps_used: int, bds_used: int):
    packed = (gps_visible & 0xFF) | ((bds_visible & 0xFF) << 8) | ((gps_used & 0xFF) << 16) | ((bds_used & 0xFF) << 24)
    _named_value_int(conn, now_ms, "GNSS_SAT", packed)


def send_humidity(conn, now_ms: int, humidity_pct_x10: int):
    """humidity_pct_x10=535 表示 53.5% 相对湿度。此前固件发的 name 是 ENVHUM，已按
    Formal_Framework PR#1 改名为 HUMIDITY，布局不变。"""
    _named_value_int(conn, now_ms, "HUMIDITY", humidity_pct_x10)


def send_lorastat(conn, now_ms: int, loss_rate_x10: int, node_id: int, present: bool, link_state: int):
    """RPi 专属，只发 USART1，不上 LoRa。link_state 跟 MODSTAT0/1 里 LORA 模块(索引4)
    的状态语义重复，但两条帧独立发送、不做一致性校验。"""
    packed = ((loss_rate_x10 & 0xFFFF) | ((node_id & 0xFF) << 16) |
              ((1 if present else 0) << 24) | ((link_state & 0x07) << 25))
    _named_value_int(conn, now_ms, "LORASTAT", packed)


def send_ridstat(conn, now_ms: int, location_count: int, error_count: int):
    """RPi 专属，只发 USART1。location_count/error_count 是增量语义的计数器低16位。
    time_boot_ms 按固件约定应为"RemoteID最近一次成功提交时间"，这里演示直接用当前时刻。"""
    packed = (location_count & 0xFFFF) | ((error_count & 0xFFFF) << 16)
    _named_value_int(conn, now_ms, "RIDSTAT", packed)


def send_open_drone_id_basic_id(conn):
    """身份帧(M3c)：厂商唯一产品识别码通过 uas_id 携带，RPi 用 strnlen 提取，
    不做格式校验，这里用 DEMO_VENDOR_ID 演示值，不是真实 SN。"""
    conn.mav.open_drone_id_basic_id_send(
        target_system=0,
        target_component=0,
        id_or_mac=ID_OR_MAC_UNUSED,
        id_type=mavutil.mavlink.MAV_ODID_ID_TYPE_SERIAL_NUMBER,
        ua_type=mavutil.mavlink.MAV_ODID_UA_TYPE_HELICOPTER_OR_MULTIROTOR,
        uas_id=DEMO_VENDOR_ID,
    )


def send_open_drone_id_location(conn):
    conn.mav.open_drone_id_location_send(
        target_system=0,
        target_component=0,
        id_or_mac=ID_OR_MAC_UNUSED,
        status=mavutil.mavlink.MAV_ODID_STATUS_AIRBORNE,
        direction=0,
        speed_horizontal=0,
        speed_vertical=0,
        latitude=int(31.2304 * 1e7),
        longitude=int(121.4737 * 1e7),
        altitude_barometric=50.0,
        altitude_geodetic=50.0,
        height_reference=mavutil.mavlink.MAV_ODID_HEIGHT_REF_OVER_TAKEOFF,
        height=0.0,
        horizontal_accuracy=mavutil.mavlink.MAV_ODID_HOR_ACC_UNKNOWN,
        vertical_accuracy=mavutil.mavlink.MAV_ODID_VER_ACC_UNKNOWN,
        barometer_accuracy=mavutil.mavlink.MAV_ODID_VER_ACC_UNKNOWN,
        speed_accuracy=mavutil.mavlink.MAV_ODID_SPEED_ACC_UNKNOWN,
        timestamp=0.0,
        timestamp_accuracy=mavutil.mavlink.MAV_ODID_TIME_ACC_UNKNOWN,
    )


def send_open_drone_id_system(conn, now_ms: int):
    conn.mav.open_drone_id_system_send(
        target_system=0,
        target_component=0,
        id_or_mac=ID_OR_MAC_UNUSED,
        operator_location_type=mavutil.mavlink.MAV_ODID_OPERATOR_LOCATION_TYPE_TAKEOFF,
        classification_type=mavutil.mavlink.MAV_ODID_CLASSIFICATION_TYPE_UNDECLARED,
        operator_latitude=int(31.2304 * 1e7),
        operator_longitude=int(121.4737 * 1e7),
        area_count=1,
        area_radius=0,
        area_ceiling=-1000.0,
        area_floor=-1000.0,
        category_eu=mavutil.mavlink.MAV_ODID_CATEGORY_EU_UNDECLARED,
        class_eu=mavutil.mavlink.MAV_ODID_CLASS_EU_UNDECLARED,
        operator_altitude_geo=-1000.0,
        timestamp=now_ms // 1000,
    )


def send_open_drone_id_operator_id(conn):
    """operator_id 目前没有真实 CAA 登记号，用占位文本演示（对应固件对接文档§3.4的开放问题）。"""
    conn.mav.open_drone_id_operator_id_send(
        target_system=0,
        target_component=0,
        id_or_mac=ID_OR_MAC_UNUSED,
        operator_id_type=mavutil.mavlink.MAV_ODID_OPERATOR_ID_TYPE_CAA,
        operator_id=b"DEMO-OPERATOR",
    )


def send_open_drone_id_self_id(conn):
    """description 目前没有真实飞行目的描述，用占位文本演示。"""
    conn.mav.open_drone_id_self_id_send(
        target_system=0,
        target_component=0,
        id_or_mac=ID_OR_MAC_UNUSED,
        description_type=mavutil.mavlink.MAV_ODID_DESC_TYPE_TEXT,
        description=b"CNS training kit demo",
    )


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
            send_battery_status(conn, battery_id=0, cell_mv=[11800],
                                 current_ca=1500, current_consumed_mah=500,
                                 energy_consumed_hj=1000, percent=80); time.sleep(msg_gap_s)
            send_battery_status(conn, battery_id=1, cell_mv=[11500],
                                 current_ca=1200, current_consumed_mah=400,
                                 energy_consumed_hj=800, percent=75); time.sleep(msg_gap_s)
            send_scaled_pressure(conn, now_ms, press_abs_hpa=1013.25, temperature_cdeg=2500); time.sleep(msg_gap_s)
            send_motor12(conn, now_ms, duty1=50, duty2=50, run_state=True, speed_level=60); time.sleep(msg_gap_s)
            send_motor34(conn, now_ms, duty3=48, duty4=52, run_state=True, speed_level=60); time.sleep(msg_gap_s)
            send_gnss_sat(conn, now_ms, gps_visible=8, bds_visible=6, gps_used=6, bds_used=4); time.sleep(msg_gap_s)
            send_humidity(conn, now_ms, humidity_pct_x10=535); time.sleep(msg_gap_s)
            send_lorastat(conn, now_ms, loss_rate_x10=15, node_id=args.sysid, present=True, link_state=STATE_ONLINE); time.sleep(msg_gap_s)
            send_ridstat(conn, now_ms, location_count=sent_rounds, error_count=0); time.sleep(msg_gap_s)
            send_tunnel_alarm_table(conn, records=[(3, 101, SEVERITY_WARNING, True, 12)]); time.sleep(msg_gap_s)  # source_id=3(BATTERY)
            send_tunnel_message_log(conn, latest_seq=sent_rounds, entries=[]); time.sleep(msg_gap_s)  # 只发心跳，不带日志条目
            send_open_drone_id_basic_id(conn); time.sleep(msg_gap_s)
            send_open_drone_id_location(conn); time.sleep(msg_gap_s)
            send_open_drone_id_system(conn, now_ms); time.sleep(msg_gap_s)
            send_open_drone_id_operator_id(conn); time.sleep(msg_gap_s)
            send_open_drone_id_self_id(conn)

            sent_rounds += 1
            print(f"[{sent_rounds}] 已发送一轮：HEARTBEAT/GPS_RAW_INT/ATTITUDE/SYS_STATUS/"
                  f"MODSTAT0/MODSTAT1/BATTERY_STATUS(id=0)/BATTERY_STATUS(id=1)/"
                  f"SCALED_PRESSURE/MOTOR12/MOTOR34/GNSS_SAT/HUMIDITY/"
                  f"LORASTAT/RIDSTAT/TUNNEL(告警表+日志心跳)/OPEN_DRONE_ID_*(5种身份帧)")

            if args.count and sent_rounds >= args.count:
                break
            time.sleep(interval)
    except KeyboardInterrupt:
        print("\n已停止")


if __name__ == "__main__":
    main()
