#!/usr/bin/env python3
"""
scripts/cellular_dialup.py —— 5G模块开机拨号脚本。

用途：把 docs/5G模块拨号验证.md 里手动敲的AT命令序列(AT+CGDCONT/AT+CGACT/
AT+QNETDEVCTL)自动化，由 systemd/cellular-dialup.service 在开机时跑一次。

跟 cns_rpi C++ 主程序完全独立(不同进程、不同语言、不进CMake构建)，只共享
config/config.json 里新增的 "cellular" 节。

设计依据：docs/superpowers/specs/2026-07-09-cellular-dialup-design.md。
"""

import json
import re
import time


AT_BAUDRATE = 115200
AT_COMMAND_TIMEOUT_SECONDS = 5
CARRIER_WAIT_SECONDS = 10
NCM_NETCARD_INDEX = 0
NCM_IFACE_NAME = "usb0"

_CGDCONT_LINE_RE = re.compile(r'^\+CGDCONT:\s*(\d+),"([^"]*)","([^"]*)"')
_CGACT_LINE_RE = re.compile(r'^\+CGACT:\s*(\d+),(\d+)')


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


if __name__ == "__main__":
    pass  # main()留给Task 3
