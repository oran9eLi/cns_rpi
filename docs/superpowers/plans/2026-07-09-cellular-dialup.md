# 5G模块开机拨号自动化 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `docs/5G模块拨号验证.md` 里手动敲的 AT 拨号序列（`AT+CGDCONT`/`AT+CGACT`/`AT+QNETDEVCTL`）封装成一个开机自动跑一次的独立 Python 脚本 + systemd oneshot 单元，跟 `cns_rpi` C++ 主程序完全独立，只共享 `config/config.json` 新增的一节。

**Architecture:** 单文件 Python 脚本 `scripts/cellular_dialup.py`，内部分两类函数：一类是纯逻辑（配置读取、AT 响应文本解析、"该不该发下一条命令/算不算成功"的判断，包括通过依赖注入的假串口对象做单测的 `send_at_command`），一类是真实 I/O（`udevadm`/`ip link` 子进程调用、真实 pyserial 端口），后者不写自动化单测，走人工真机验证——这个边界跟 C++ 那边`uart::MavlinkLink`对真实串口的处理方式一致。

**Tech Stack:** Python 3 标准库 + `pyserial`（AT 串口收发），`unittest`（自带，不引入 pytest），systemd（oneshot unit）。

## Global Constraints

- 不引入 pytest 等新依赖，测试用 Python 标准库自带的 `unittest`。
- 脚本本身不做多次重试——`Open()`/单条 AT 命令失败就整体退出非 0，重试策略交给 systemd 的 `Restart=on-failure`（不在脚本里写循环重试整个拨号序列）。
- 不集成进 `cns_rpi` C++ 构建（不进 CMakeLists.txt），不复用 `uart::SerialPort`。
- 提交信息格式 `<type>: <简短中文说明>`，中文描述，不写长正文（`docs/协作规则.md`）。
- 新文件按 Python 惯例写文件头 docstring 说明这个文件负责什么（对应 C++ 那边 Doxygen 文件头注释的角色）。
- 依据：`docs/superpowers/specs/2026-07-09-cellular-dialup-design.md`（本计划的设计文档，所有字段/行为细节以它为准）。

---

## Task 1: 纯逻辑——配置读取 + AT 响应解析/判断 + `send_at_command`

这是本计划唯一有自动化单测的部分：配置读取、`AT+CGDCONT?`/`AT+CGACT?` 响应解析、"是否需要发`CGDCONT=`"和"CGACT失败后该不该判定成功"的决策逻辑，以及`send_at_command`（虽然涉及"发送/接收"，但因为接的是传进来的对象而不是真实串口，可以用一个假对象单测，覆盖 OK/ERROR/+CME ERROR/超时四种情况）。

**Files:**
- Create: `scripts/cellular_dialup.py`（本任务只写这部分函数，`main()`/真实I/O函数留给 Task 2/3）
- Create: `scripts/test_cellular_dialup.py`
- Modify: `config/config.example.json`（新增 `cellular` 节）

**Interfaces:**
- Produces：`load_cellular_config(path) -> dict`（keys: `apn`/`cid`/`usb_interface_number`/`at_port_wait_seconds`）、`parse_cgdcont_apn(lines, cid) -> str | None`、`needs_cgdcont_set(existing_apn, desired_apn) -> bool`、`parse_cgact_active(lines, cid) -> bool`、`send_at_command(ser, command, timeout=5) -> tuple[bool, list[str]]`——Task 3 会调用这五个。

- [ ] **Step 1: 写 `scripts/cellular_dialup.py`（本任务范围内的部分）**

```python
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
```

- [ ] **Step 2: 写 `scripts/test_cellular_dialup.py`**

```python
#!/usr/bin/env python3
"""scripts/cellular_dialup.py 的单元测试，只覆盖纯逻辑部分(不碰真实串口/子进程)。
运行：cd scripts && python3 -m unittest test_cellular_dialup -v
"""

import json
import os
import tempfile
import unittest

from cellular_dialup import (
    load_cellular_config,
    needs_cgdcont_set,
    parse_cgact_active,
    parse_cgdcont_apn,
    send_at_command,
)


class FakeSerial:
    """假串口：readline()按顺序弹出预设的响应行(每行末尾自动补\\r\\n再编码)，
    弹空之后返回b""模拟没有新数据；write()/reset_input_buffer()只记录调用。"""

    def __init__(self, response_lines):
        self._lines = list(response_lines)
        self.written = []

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.written.append(data)

    def readline(self):
        if self._lines:
            return (self._lines.pop(0) + "\r\n").encode("ascii")
        return b""


class TestLoadCellularConfig(unittest.TestCase):
    def test_读取完整的cellular节(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            json.dump({
                "cellular": {
                    "apn": "cmnet",
                    "cid": 1,
                    "usb_interface_number": "05",
                    "at_port_wait_seconds": 30,
                }
            }, f)
            path = f.name
        try:
            cfg = load_cellular_config(path)
            self.assertEqual(cfg["apn"], "cmnet")
            self.assertEqual(cfg["cid"], 1)
            self.assertEqual(cfg["usb_interface_number"], "05")
            self.assertEqual(cfg["at_port_wait_seconds"], 30)
        finally:
            os.unlink(path)

    def test_缺cellular节抛KeyError(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            json.dump({}, f)
            path = f.name
        try:
            with self.assertRaises(KeyError):
                load_cellular_config(path)
        finally:
            os.unlink(path)


class TestParseCgdcontApn(unittest.TestCase):
    def test_找到匹配的cid返回对应APN(self):
        lines = [
            '+CGDCONT: 1,"IPV4V6","cmnet","0.0.0.0",0,0,0,0',
            '+CGDCONT: 11,"IPV4V6","ims","0.0.0.0",0,0,0,2',
        ]
        self.assertEqual(parse_cgdcont_apn(lines, 1), "cmnet")
        self.assertEqual(parse_cgdcont_apn(lines, 11), "ims")

    def test_找不到匹配的cid返回None(self):
        lines = ['+CGDCONT: 11,"IPV4V6","ims","0.0.0.0",0,0,0,2']
        self.assertIsNone(parse_cgdcont_apn(lines, 1))

    def test_空响应返回None(self):
        self.assertIsNone(parse_cgdcont_apn([], 1))


class TestNeedsCgdcontSet(unittest.TestCase):
    def test_APN一致不需要设置(self):
        self.assertFalse(needs_cgdcont_set("cmnet", "cmnet"))

    def test_APN不一致需要设置(self):
        self.assertTrue(needs_cgdcont_set("cmnet", "iot-apn"))

    def test_cid不存在(None)需要设置(self):
        self.assertTrue(needs_cgdcont_set(None, "cmnet"))


class TestParseCgactActive(unittest.TestCase):
    def test_cid已激活返回True(self):
        lines = ["+CGACT: 1,1", "+CGACT: 11,0"]
        self.assertTrue(parse_cgact_active(lines, 1))

    def test_cid未激活返回False(self):
        lines = ["+CGACT: 1,1", "+CGACT: 11,0"]
        self.assertFalse(parse_cgact_active(lines, 11))

    def test_找不到cid返回False(self):
        self.assertFalse(parse_cgact_active(["+CGACT: 11,0"], 1))


class TestSendAtCommand(unittest.TestCase):
    def test_正常收到OK(self):
        ser = FakeSerial(["OK"])
        ok, lines = send_at_command(ser, "AT+CPIN?")
        self.assertTrue(ok)
        self.assertEqual(lines, [])
        self.assertEqual(ser.written, [b"AT+CPIN?\r\n"])

    def test_带数据行的OK响应(self):
        ser = FakeSerial(['+CGDCONT: 1,"IPV4V6","cmnet","0.0.0.0",0,0,0,0', "OK"])
        ok, lines = send_at_command(ser, "AT+CGDCONT?")
        self.assertTrue(ok)
        self.assertEqual(lines, ['+CGDCONT: 1,"IPV4V6","cmnet","0.0.0.0",0,0,0,0'])

    def test_ERROR响应(self):
        ser = FakeSerial(["ERROR"])
        ok, lines = send_at_command(ser, "AT+FOO")
        self.assertFalse(ok)
        self.assertEqual(lines, [])

    def test_CME_ERROR响应(self):
        ser = FakeSerial(["+CME ERROR: 4"])
        ok, lines = send_at_command(ser, "AT+FOO")
        self.assertFalse(ok)

    def test_超时没有终止行(self):
        ser = FakeSerial([])  # readline()永远返回b""
        ok, lines = send_at_command(ser, "AT+FOO", timeout=0.1)
        self.assertFalse(ok)
        self.assertEqual(lines, [])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 3: 改 `config/config.example.json`**

文件末尾几行现在是：

```json
    "file": ""
  },
  "identity": {
    "school_name": "NNUTC"
  }
}
```

改成：

```json
    "file": ""
  },
  "identity": {
    "school_name": "NNUTC"
  },
  "cellular": {
    "apn": "cmnet",
    "cid": 1,
    "usb_interface_number": "05",
    "at_port_wait_seconds": 30
  }
}
```

- [ ] **Step 4: 运行测试**

```bash
cd scripts && python3 -m unittest test_cellular_dialup -v
```

Expected: 全部 `TEST_CASE`（16 条）PASS，无警告。

- [ ] **Step 5: Commit**

```bash
git add scripts/cellular_dialup.py scripts/test_cellular_dialup.py config/config.example.json
git commit -m "$(cat <<'EOF'
feat: 新增cellular_dialup纯逻辑部分(配置读取+AT响应解析+决策)

5G拨号自动化第一步：config.json新增cellular节读取、AT+CGDCONT?/
AT+CGACT?响应解析、"要不要发CGDCONT="和send_at_command的OK/ERROR/
+CME ERROR/超时判断，全部用假对象/临时文件单测覆盖，不碰真实串口。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: 真实 I/O——找 AT 口 + 等网卡起来

**Files:**
- Modify: `scripts/cellular_dialup.py`（追加函数，不改 Task 1 已写的部分）

**Interfaces:**
- Consumes：无（这两个函数只用标准库 `subprocess`/`glob`/`time`）。
- Produces：`find_at_port(interface_number, wait_seconds) -> str | None`、`wait_for_carrier(iface, timeout_seconds=CARRIER_WAIT_SECONDS) -> bool`——Task 3 会调用这两个。

本任务不写自动化单测——真实调用 `udevadm`/`ip link`，跟 C++ 那边 `uart::MavlinkLink` 对真实串口一样不写自动化测试，验收标准是"能正常导入、语法正确"，真实行为留给 Task 5 的人工真机验证。

- [ ] **Step 1: 在 `scripts/cellular_dialup.py` 顶部 `import` 区追加**

把：

```python
import json
import re
import time
```

改成：

```python
import glob
import json
import re
import subprocess
import time
```

- [ ] **Step 2: 追加 `find_at_port`/`wait_for_carrier`**

在 `send_at_command` 函数后面（`if __name__ == "__main__":` 之前）追加：

```python
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
```

- [ ] **Step 3: 语法/导入检查**

```bash
python3 -m py_compile scripts/cellular_dialup.py
cd scripts && python3 -m unittest test_cellular_dialup -v
```

Expected: 编译无错误；Task 1 的 16 条测试仍然全部 PASS（本任务没有改动它们依赖的函数）。

- [ ] **Step 4: Commit**

```bash
git add scripts/cellular_dialup.py
git commit -m "$(cat <<'EOF'
feat: cellular_dialup追加真实I/O部分(找AT口+等网卡起来)

find_at_port靠udevadm的ID_USB_INTERFACE_NUM属性识别AT口(设备号会随
USB插拔变化，不能用固定ttyUSB编号)；wait_for_carrier轮询ip link
show等LOWER_UP，DHCP交给NetworkManager不用脚本管。真实调用外部
命令，不写自动化单测，跟uart::MavlinkLink对真实串口的处理方式一致。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `dial_up()` + `main()` 编排

**Files:**
- Modify: `scripts/cellular_dialup.py`

**Interfaces:**
- Consumes：Task 1 的 `load_cellular_config`/`parse_cgdcont_apn`/`needs_cgdcont_set`/`parse_cgact_active`/`send_at_command`；Task 2 的 `find_at_port`/`wait_for_carrier`。
- Produces：`dial_up(config) -> bool`、`main()`（脚本命令行入口，`python3 cellular_dialup.py <config.json路径>`）。

本任务不写自动化单测（编排逻辑本身，跟 C++ 那边 `main.cpp` 从来没有单测是同样的边界），验收标准是能正常导入 + Task 5 的人工真机验证。

- [ ] **Step 1: 追加 `pyserial` import**

把：

```python
import glob
import json
import re
import subprocess
import time
```

改成：

```python
import glob
import json
import re
import subprocess
import sys
import time

import serial
```

- [ ] **Step 2: 追加 `dial_up()`/`main()`**

在 `wait_for_carrier` 函数后面（`if __name__ == "__main__":` 之前）追加：

```python
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

    with serial.Serial(port, AT_BAUDRATE, timeout=1) as ser:
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

    print(f"[cellular_dialup] 拨号成功，{NCM_IFACE_NAME}已起来，DHCP交给NetworkManager处理")
    return True


def main():
    if len(sys.argv) != 2:
        print(f"用法: {sys.argv[0]} <config.json路径>", file=sys.stderr)
        sys.exit(2)
    config = load_cellular_config(sys.argv[1])
    success = dial_up(config)
    sys.exit(0 if success else 1)
```

把文件末尾的：

```python
if __name__ == "__main__":
    pass  # main()留给Task 3
```

改成：

```python
if __name__ == "__main__":
    main()
```

- [ ] **Step 3: 语法检查 + 回归测试**

```bash
python3 -m py_compile scripts/cellular_dialup.py
cd scripts && python3 -m unittest test_cellular_dialup -v
```

Expected: 编译无错误；Task 1 的 16 条测试全部 PASS。

- [ ] **Step 4: Commit**

```bash
git add scripts/cellular_dialup.py
git commit -m "$(cat <<'EOF'
feat: cellular_dialup补齐dial_up()编排+main()命令行入口

按设计文档顺序串起来:CGDCONT?检查->按需CGDCONT=->CGACT(失败则
CGACT?复查)->QNETDEVCTL->等usb0 LOWER_UP。main()读参数指定的
config.json，成功exit 0/失败exit非0，重试交给systemd。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: systemd 单元 + 环境依赖

**Files:**
- Create: `systemd/cellular-dialup.service`
- Modify: `scripts/install_deps.sh`

**Interfaces:** 无（部署配置，不被其他代码依赖）。

- [ ] **Step 1: 写 `systemd/cellular-dialup.service`**

```ini
[Unit]
Description=cns_rpi 5G模块开机拨号(AT+CGDCONT/CGACT/QNETDEVCTL)
After=network-pre.target
Before=network.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/bin/python3 /opt/cns_rpi/scripts/cellular_dialup.py /opt/cns_rpi/config/config.json
Restart=on-failure
RestartSec=15

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 2: 改 `scripts/install_deps.sh`，加装 `python3-serial`**

把：

```bash
sudo apt install -y build-essential cmake git nlohmann-json3-dev doctest-dev
```

改成：

```bash
sudo apt install -y build-essential cmake git nlohmann-json3-dev doctest-dev python3-serial
```

同时把文件顶部横幅（`cat <<'BANNER' ... BANNER`）里第 `[2]` 条的说明追加一句，把：

```
  [2] 安装构建依赖 -> build-essential cmake git nlohmann-json3-dev doctest-dev
      （nlohmann-json3-dev 是配置文件解析用的头文件库；doctest-dev 是单元测试框架，只在开发机/CI需要，
      不影响 systemd 部署的运行时依赖）
```

改成：

```
  [2] 安装构建依赖 -> build-essential cmake git nlohmann-json3-dev doctest-dev python3-serial
      （nlohmann-json3-dev 是配置文件解析用的头文件库；doctest-dev 是单元测试框架，只在开发机/CI需要，
      不影响 systemd 部署的运行时依赖；python3-serial 是 scripts/cellular_dialup.py 拨号脚本
      运行时依赖，真机部署必须装）
```

- [ ] **Step 3: Commit**

```bash
git add systemd/cellular-dialup.service scripts/install_deps.sh
git commit -m "$(cat <<'EOF'
build: 新增cellular-dialup systemd单元+python3-serial依赖

Type=oneshot+RemainAfterExit=yes，不依赖还没造出来的cns-rpi.service
(M7才建)，5G拨号跟MAVLink/UART链路完全独立、互不阻塞。失败交给
Restart=on-failure按15秒间隔重试。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: 文档同步 + 真机人工验证

**Files:**
- Modify: `docs/V1设计文档.md`

**Interfaces:** 无（纯文档 + 人工验证）。

- [ ] **Step 1: 更新 `docs/V1设计文档.md`**

在 §10 里程碑列表的 M7 那一行（`- **M7 系统化部署**：systemd 服务化...`）后面新增一行（这个能力独立于 M7 的 `cns-rpi.service`，但同属"systemd 部署"这类工作，放在附近方便查找）：

```markdown
- **5G 模块开机拨号自动化**：`scripts/cellular_dialup.py` + `systemd/cellular-dialup.service`，把 `docs/5G模块拨号验证.md` 里的手动 AT 命令序列自动化，独立于 `cns_rpi` 主程序——已实现，见 `docs/superpowers/plans/2026-07-09-cellular-dialup.md`
```

- [ ] **Step 2: Commit 文档改动**

```bash
git add docs/V1设计文档.md
git commit -m "$(cat <<'EOF'
docs: 同步5G拨号自动化落地状态到V1设计文档

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 3: 人工验证（需要真机 SSH 访问，真机是 `dcdw@192.168.11.4`）**

1. 把改动同步到真机（推到 GitHub 后 `git pull`，或直接 `scp`，具体方式跟之前 M5 真机验证一致，需要用户确认走哪条路径）。
2. 装依赖：`sudo apt install python3-serial`（如果真机还没装）。
3. 不装 systemd 单元，先手动跑一次：

```bash
python3 scripts/cellular_dialup.py config/config.json
```

对照 `docs/5G模块拨号验证.md` 里已经手动验证过的每一步，确认：
   - 脚本打出"找到AT口: /dev/ttyUSBx"；
   - 不报 `AT+CGDCONT?`/`AT+CGACT`/`AT+QNETDEVCTL` 任一步失败；
   - `ip link show usb0` 显示 `LOWER_UP`；
   - `ip addr show usb0` 能看到 NetworkManager 自动分配的 IP；
   - `ping -I usb0 8.8.8.8` 能通。

4. 确认没问题后，把 `config/config.json` 的 `cellular.usb_interface_number` 按真机当前实际值核对一次（真机之前验证的是 `"05"`，如果这次环境不同要更新)。
5. 安装并启用 systemd 单元：

```bash
sudo cp systemd/cellular-dialup.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now cellular-dialup.service
sudo systemctl status cellular-dialup.service
```

Expected: `Active: active (exited)`。

6. 重启真机，确认开机后 5G 自动拨号成功（重复步骤 3 的连通性检查，不需要再手动跑脚本）。

**已知待验证项**（见设计文档第4节"假设待验证"）：这次验证用的是手机卡，`AT+CGDCONT=`手动建上下文这条分支没有被真正触发过（这次是预置好的上下文）。等正式物联卡到货后，需要重新走一遍上面的验证，同时确认这条分支的实际行为。

## Self-Review 记录

- **Spec 覆盖检查**：设计文档 §2（文件布局）→ Task 1/4；§3（config schema）→ Task 1 Step 3；§4（拨号序列）→ Task 1（解析/决策）+ Task 2（真实I/O）+ Task 3（编排）；§5（systemd单元）→ Task 4 Step 1；§6（错误处理）→ Task 3 的 `dial_up()` 每一步失败即返回False，体现在代码里；§7（测试计划）→ Task 1 的单测覆盖纯逻辑，Task 5 的人工验证覆盖真实I/O和端到端行为。全部覆盖。
- **占位符扫描**：没有 TBD/TODO，每个 Step 都是完整代码。"假设待验证"的 `AT+CGDCONT=` 分支在 Task 5 里明确列为已知待验证项，不是遗漏。
- **类型一致性检查**：`load_cellular_config` 返回的 dict 键名（`apn`/`cid`/`usb_interface_number`/`at_port_wait_seconds`）在 Task 1 声明、Task 3 `dial_up()` 使用，完全一致。`send_at_command`/`find_at_port`/`wait_for_carrier`/`parse_cgdcont_apn`/`needs_cgdcont_set`/`parse_cgact_active` 的函数名和参数在跨 Task 引用处（Task 3 调用 Task 1/2 的函数）保持一致。
- **范围检查**：5 个 Task 都在这个子项目范围内，不涉及 ModemManager、DHCP 脚本、路由 metric 调整、PIN 解锁，也不碰 `cns_rpi` C++ 代码/CMake——符合设计文档"明确不做的事"。
