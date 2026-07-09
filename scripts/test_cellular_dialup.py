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

    def test_cid不存在需要设置(self):
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
