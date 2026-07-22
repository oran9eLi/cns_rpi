#!/usr/bin/env python3
"""
scripts/cellular_dialup.py —— 5G模块开机拨号脚本。

用途：把 docs/5G模块拨号验证.md 里手动敲的AT命令序列(AT+CGDCONT/AT+CGACT/
AT+QNETDEVCTL)自动化，由 systemd/cellular-dialup.service 在开机时跑一次。

跟 cns_rpi C++ 主程序完全独立(不同进程、不同语言、不进CMake构建)，只共享
config/config.json 里新增的 "cellular" 节。

设计依据：docs/superpowers/specs/2026-07-09-cellular-dialup-design.md。
"""

import dataclasses
import glob
import json
import os
import pathlib
import re
import subprocess
import sys
import time
from typing import Optional

import serial

from scripts.cellular_link import (
    parse_cops,
    parse_csq,
    parse_qeng,
)


AT_BAUDRATE = 115200
AT_COMMAND_TIMEOUT_SECONDS = 5
CARRIER_WAIT_SECONDS = 10
NCM_NETCARD_INDEX = 0
NCM_IFACE_NAME = "usb0"

_CGDCONT_LINE_RE = re.compile(r'^\+CGDCONT:\s*(\d+),"([^"]*)","([^"]*)"')
_CGACT_LINE_RE = re.compile(r'^\+CGACT:\s*(\d+),(\d+)')


@dataclasses.dataclass(frozen=True)
class InterfaceStatus:
    """Linux 数据接口的基础链路和流量状态。"""

    interface_present: bool = False
    carrier_up: bool = False
    has_ip_address: bool = False
    has_default_route: bool = False
    ip_address: Optional[str] = None
    tx_bytes: Optional[int] = None
    rx_bytes: Optional[int] = None

    @property
    def basic_ready(self):
        return (
            self.interface_present
            and self.carrier_up
            and self.has_ip_address
            and self.has_default_route
        )


@dataclasses.dataclass(frozen=True)
class RadioSample:
    """单次无线质量采集结果。"""

    operator: Optional[str] = None
    access_technology: Optional[str] = None
    rsrp_dbm: Optional[int] = None
    rsrq_db: Optional[int] = None
    sinr_db: Optional[int] = None
    rssi_dbm: Optional[int] = None
    error: Optional[str] = None


def load_cellular_config(path):
    """读取config.json，取出cellular节。缺字段直接抛KeyError，脚本层面
    在main()里统一捕获、打日志、退出非0，不做C++那边std::expected式的
    细分错误类型——这是独立小脚本，不需要那么重的错误处理。"""
    with open(path, "r", encoding="utf-8") as f:
        root = json.load(f)
    cellular = root["cellular"]
    return {
        "apn": cellular["apn"],
        "cid": int(cellular["cid"]),
        "usb_interface_number": cellular["usb_interface_number"],
        "at_port_wait_seconds": int(cellular["at_port_wait_seconds"]),
    }


def parse_cgdcont_apn(lines, cid):
    """从AT+CGDCONT?响应的每一行文本里找cid对应的APN，没找到返回None。
    lines形如 '+CGDCONT: 1,"IPV4V6","cmnet","0.0.0.0",0,0,0,0'（不含OK那行）。
    """
    for line in lines:
        m = _CGDCONT_LINE_RE.match(line.strip())
        if m and int(m.group(1)) == cid:
            return m.group(3)
    return None


def needs_cgdcont_set(existing_apn, desired_apn):
    """existing_apn是parse_cgdcont_apn的返回值(可能是None，代表cid压根不存在)。"""
    return existing_apn != desired_apn


def parse_cgact_active(lines, cid):
    """从AT+CGACT?响应的每一行文本里判断cid是否已激活。
    lines形如 '+CGACT: 1,1'(第二个数字1=已激活,0=未激活)。找不到这个cid也返回False。
    """
    for line in lines:
        m = _CGACT_LINE_RE.match(line.strip())
        if m and int(m.group(1)) == cid:
            return m.group(2) == "1"
    return False


def send_at_command(ser, command, timeout=AT_COMMAND_TIMEOUT_SECONDS):
    """发一条AT命令，逐行读回应直到OK/ERROR/+CME ERROR或超时。
    ser只要求有reset_input_buffer()/write(bytes)/readline()->bytes三个方法
    (真实pyserial.Serial满足，测试用假对象也满足，不依赖pyserial具体类型)。
    返回(ok, lines)：ok表示是否收到OK；lines是过程中收到的非终止行(不含
    OK/ERROR/+CME ERROR那一行本身)。
    """
    ser.reset_input_buffer()
    ser.write((command + "\r\n").encode("ascii"))
    lines = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("ascii", errors="ignore").strip()
        if not line:
            continue
        if line == "OK":
            return True, lines
        if line == "ERROR" or line.startswith("+CME ERROR"):
            return False, lines
        lines.append(line)
    return False, lines


def collect_radio_metrics(ser, sender=send_at_command):
    """采集运营商和无线质量；单条 AT 查询失败不影响其他指标。"""

    errors = []

    cops_ok, cops_lines = sender(ser, "AT+COPS?")
    operator = parse_cops(cops_lines) if cops_ok else None
    if not cops_ok:
        errors.append("COPS")

    qeng_ok, qeng_lines = sender(ser, 'AT+QENG="servingcell"')
    metrics = parse_qeng(qeng_lines) if qeng_ok else None
    if not qeng_ok:
        errors.append("QENG")

    csq_ok, csq_lines = sender(ser, "AT+CSQ")
    rssi_dbm = parse_csq(csq_lines) if csq_ok else None
    if not csq_ok:
        errors.append("CSQ")

    return RadioSample(
        operator=operator,
        access_technology=(metrics.access_technology if metrics else None),
        rsrp_dbm=(metrics.rsrp_dbm if metrics else None),
        rsrq_db=(metrics.rsrq_db if metrics else None),
        sinr_db=(metrics.sinr_db if metrics else None),
        rssi_dbm=rssi_dbm,
        error=(f"{'、'.join(errors)}查询失败" if errors else None),
    )


def probe_target(interface_name, target, timeout_seconds=2, runner=subprocess.run):
    """强制通过指定接口执行一次公网 ICMP 探测。"""

    command = [
        "ping",
        "-I",
        interface_name,
        "-c",
        "1",
        "-W",
        str(timeout_seconds),
        target,
    ]
    try:
        result = runner(
            command,
            capture_output=True,
            text=True,
            timeout=timeout_seconds + 2,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return result.returncode == 0


def probe_interface(interface_name, net_root=pathlib.Path("/sys/class/net"),
                    runner=subprocess.run):
    """读取指定 Linux 网卡的载波、地址、路由和累计流量。"""

    interface_path = pathlib.Path(net_root) / interface_name
    if not interface_path.exists():
        return InterfaceStatus()

    carrier_up = _read_text(interface_path / "carrier") == "1"
    tx_bytes = _read_nonnegative_int(interface_path / "statistics" / "tx_bytes")
    rx_bytes = _read_nonnegative_int(interface_path / "statistics" / "rx_bytes")

    ip_address = None
    try:
        address_result = runner(
            ["ip", "-j", "address", "show", "dev", interface_name],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        if address_result.returncode == 0:
            ip_address = _first_global_ip(address_result.stdout)
    except (OSError, subprocess.TimeoutExpired):
        pass

    has_default_route = False
    try:
        route_result = runner(
            ["ip", "route", "show", "default", "dev", interface_name],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        has_default_route = (
            route_result.returncode == 0
            and any(
                line.startswith("default ")
                for line in route_result.stdout.splitlines()
            )
        )
    except (OSError, subprocess.TimeoutExpired):
        pass

    return InterfaceStatus(
        interface_present=True,
        carrier_up=carrier_up,
        has_ip_address=ip_address is not None,
        has_default_route=has_default_route,
        ip_address=ip_address,
        tx_bytes=tx_bytes,
        rx_bytes=rx_bytes,
    )


def write_snapshot_atomic(path, snapshot):
    """在同一目录写入临时文件后原子替换正式快照。"""

    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    try:
        temporary.write_text(
            json.dumps(
                snapshot.to_dict(),
                ensure_ascii=False,
                separators=(",", ":"),
            )
            + "\n",
            encoding="utf-8",
        )
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _read_text(path):
    try:
        return path.read_text(encoding="ascii").strip()
    except OSError:
        return None


def _read_nonnegative_int(path):
    value = _read_text(path)
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return None
    return parsed if parsed >= 0 else None


def _first_global_ip(payload):
    try:
        interfaces = json.loads(payload)
    except (json.JSONDecodeError, TypeError):
        return None
    for interface in interfaces:
        for address in interface.get("addr_info", []):
            if address.get("scope") == "global" and address.get("family") in (
                "inet",
                "inet6",
            ):
                local = address.get("local")
                if isinstance(local, str) and local:
                    return local
    return None


def find_at_port(interface_number, wait_seconds):
    """轮询/dev/ttyUSB*，用udevadm找ID_USB_INTERFACE_NUM匹配interface_number
    的设备。每1秒重新扫描一次，最多等wait_seconds秒，找到返回设备路径，
    超时返回None。设备号会随其它USB设备插拔变化(docs/5G模块拨号验证.md已
    确认)，不能假设固定的ttyUSB编号，必须靠这个属性识别。
    """
    deadline = time.monotonic() + wait_seconds
    while time.monotonic() < deadline:
        for dev in sorted(glob.glob("/dev/ttyUSB*")):
            try:
                result = subprocess.run(
                    ["udevadm", "info", "-q", "property", "-n", dev],
                    capture_output=True, text=True, timeout=5, check=True,
                )
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                continue
            target = f"ID_USB_INTERFACE_NUM={interface_number}"
            if target in result.stdout.splitlines():
                return dev
        time.sleep(1)
    return None


def wait_for_carrier(iface, timeout_seconds=CARRIER_WAIT_SECONDS):
    """轮询`ip link show <iface>`直到看到LOWER_UP，超时返回False。
    DHCP不用这个脚本管——NetworkManager一见carrier就自动接管
    (docs/5G模块拨号验证.md已确认)。
    """
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            result = subprocess.run(
                ["ip", "link", "show", iface],
                capture_output=True, text=True, timeout=5, check=True,
            )
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            time.sleep(1)
            continue
        if "LOWER_UP" in result.stdout:
            return True
        time.sleep(1)
    return False


def dial_up(config):
    """执行一次完整拨号序列。返回True表示成功，False表示失败(具体原因
    已经打印到stdout，不用异常传递失败原因——这是独立小脚本，退出码
    足够表达成败，不需要更复杂的错误类型)。
    """
    port = find_at_port(config["usb_interface_number"], config["at_port_wait_seconds"])
    if port is None:
        print(f"[cellular_dialup] 超时({config['at_port_wait_seconds']}秒)没找到AT口"
              f"(interface_number={config['usb_interface_number']})")
        return False
    print(f"[cellular_dialup] 找到AT口: {port}")

    cid = config["cid"]
    apn = config["apn"]

    try:
        ser = serial.Serial(port, AT_BAUDRATE, timeout=1)
    except serial.SerialException as exc:
        print(f"[cellular_dialup] 打开串口{port}失败: {exc}")
        return False

    with ser:
        ok, lines = send_at_command(ser, "AT+CGDCONT?")
        if not ok:
            print("[cellular_dialup] AT+CGDCONT? 失败")
            return False
        existing_apn = parse_cgdcont_apn(lines, cid)
        if needs_cgdcont_set(existing_apn, apn):
            print(f"[cellular_dialup] cid={cid} 当前APN={existing_apn!r}，"
                  f"跟配置的{apn!r}不一致，发送AT+CGDCONT=设置")
            ok, _ = send_at_command(ser, f'AT+CGDCONT={cid},"IPV4V6","{apn}"')
            if not ok:
                print("[cellular_dialup] AT+CGDCONT= 失败")
                return False

        ok, _ = send_at_command(ser, f"AT+CGACT=1,{cid}")
        if not ok:
            print("[cellular_dialup] AT+CGACT 返回ERROR，用AT+CGACT?复查激活状态")
            query_ok, query_lines = send_at_command(ser, "AT+CGACT?")
            if not query_ok or not parse_cgact_active(query_lines, cid):
                print(f"[cellular_dialup] cid={cid} 确认未激活，AT+CGACT失败")
                return False
            print(f"[cellular_dialup] cid={cid} 已经处于激活状态，视为成功")

        ok, _ = send_at_command(ser, f"AT+QNETDEVCTL={cid},1,{NCM_NETCARD_INDEX}")
        if not ok:
            print("[cellular_dialup] AT+QNETDEVCTL 失败")
            return False

    if not wait_for_carrier(NCM_IFACE_NAME):
        print(f"[cellular_dialup] 等待{NCM_IFACE_NAME}出现LOWER_UP超时"
              f"({CARRIER_WAIT_SECONDS}秒)")
        return False

    print(f"[cellular_dialup] 拨号成功，{NCM_IFACE_NAME}已出现载波，DHCP交由NetworkManager")
    return True


def main():
    if len(sys.argv) != 2:
        print(f"用法: {sys.argv[0]} <config.json路径>", file=sys.stderr)
        sys.exit(2)
    try:
        config = load_cellular_config(sys.argv[1])
    except (OSError, KeyError, ValueError) as exc:
        print(f"[cellular_dialup] 读取配置失败: {exc}", file=sys.stderr)
        sys.exit(1)
    success = dial_up(config)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
