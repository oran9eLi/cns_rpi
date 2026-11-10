"""真实进程、伪串口和独立 Broker 联调；仅为模拟链路证据。"""
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import pty
import queue
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import tty
import unittest

BINARY, FIXTURE = sys.argv[1:3]
del sys.argv[1:3]
ROOT = Path(__file__).resolve().parents[1]
BOX_ID = "DCDWCNS1RTKTEST00001"
PX4_ID = "RTKINDOORPX4"
BOX_TOPIC = f"rtkindoor/{BOX_ID}"


def ubx(cls, ident, data=b""):
    body = bytes([cls, ident]) + struct.pack("<H", len(data)) + data
    a = b = 0
    for value in body:
        a = (a + value) & 255
        b = (b + a) & 255
    return b"\xb5\x62" + body + bytes([a, b])


class Receiver:
    """只模拟 F9P 的外部线协议；应用层解析、校验和判断均由真实二进制执行。"""
    def __init__(self, master):
        self.master = master
        self.values = {}
        self.survey_starts = 0
        self.survey_complete = False
        self.configuration_layers = []
        self.time_mode = self.navigation_mode = 0
        self.navigation_epoch = -1
        self.started_at = time.monotonic()
        self.waiting_for_stop = self.enabled_before_stop = False
        self.disable_ack_delay_s = 0
        self.delayed_ack_sent = threading.Event()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def respond(self, cls, ident, data):
        if (cls, ident) == (0x0A, 0x04):
            text = lambda value, length: value.encode().ljust(length, b"\0")
            value = text("HPG 1.32", 30) + text("00190000", 10)
            value += text("PROTVER=27.31", 30) + text("MOD=ZED-F9P", 30)
            os.write(self.master, ubx(cls, ident, value))
        elif (cls, ident) == (1, 0x3B):
            epoch = int(time.monotonic() - self.started_at)
            if epoch != self.navigation_epoch:
                self.navigation_epoch, self.navigation_mode = epoch, self.time_mode
                if self.navigation_mode == 0:
                    self.waiting_for_stop = False
            value = bytearray(40)
            struct.pack_into("<IIiii", value, 4, 123000 + epoch * 1000,
                             3 if self.navigation_mode else 0, 100000000, 200000000, 300000000)
            struct.pack_into("<II", value, 28, 40000, 3)
            if self.survey_complete:
                struct.pack_into("<I", value, 8, 120)
                value[36] = 1
            value[37] = int(self.navigation_mode == 1 and not self.survey_complete)
            os.write(self.master, ubx(cls, ident, value))
        elif (cls, ident) == (6, 0x8A):
            self.configuration_layers.append(data[1])
            cursor = 4
            disabling = False
            while cursor < len(data):
                key = struct.unpack_from("<I", data, cursor)[0]
                size = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8}[(key >> 28) & 7]
                self.values[key] = data[cursor + 4:cursor + 4 + size]
                if key == 0x20030001 and self.values[key] == b"\x01":
                    self.survey_starts += 1
                    self.time_mode = 1
                    self.enabled_before_stop |= self.waiting_for_stop
                if key == 0x20030001 and self.values[key] == b"\x00":
                    disabling = True
                    self.time_mode = 0
                    self.waiting_for_stop = self.navigation_mode == 1
                cursor += 4 + size
            if disabling and self.disable_ack_delay_s:
                time.sleep(self.disable_ack_delay_s)
            os.write(self.master, ubx(5, 1, bytes([cls, ident])))
            if disabling and self.disable_ack_delay_s:
                self.delayed_ack_sent.set()
        elif (cls, ident) == (6, 0x8B):
            value = b"\x01\x00\x00\x00"
            for cursor in range(4, len(data), 4):
                key = struct.unpack_from("<I", data, cursor)[0]
                if key in self.values:
                    value += struct.pack("<I", key) + self.values[key]
            os.write(self.master, ubx(cls, ident, value))

    def run(self):
        buffer = b""
        while not self.stop.is_set():
            if not select.select([self.master], [], [], 0.05)[0]:
                continue
            try:
                buffer += os.read(self.master, 4096)
            except OSError:
                continue
            while b"\xb5\x62" in buffer:
                buffer = buffer[buffer.index(b"\xb5\x62"):]
                if len(buffer) < 6:
                    break
                length = struct.unpack_from("<H", buffer, 4)[0] + 8
                if len(buffer) < length:
                    break
                frame, buffer = buffer[:length], buffer[length:]
                self.respond(frame[2], frame[3], frame[6:-2])


class GatewayTest(unittest.TestCase):
    def test_real_process_identity_base_observation_commands_and_staleness(self):
        if not shutil.which("mosquitto") or not shutil.which("mosquitto_sub"):
            self.skipTest("需要独立本地 mosquitto 与 mosquitto-clients")
        with tempfile.TemporaryDirectory(prefix="cns-rtk-gateway-") as directory:
            folder = Path(directory)
            port_socket = socket.socket()
            port_socket.bind(("127.0.0.1", 0))
            port = port_socket.getsockname()[1]
            port_socket.close()
            (folder / "broker.conf").write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
            broker_log = (folder / "broker.log").open("w+")
            logs = [broker_log]
            broker = subprocess.Popen(["mosquitto", "-c", str(folder / "broker.conf")], stdout=broker_log, stderr=broker_log)
            processes, descriptors = [broker], []
            stop_box, stop_px4 = threading.Event(), threading.Event()
            streamers = []
            receiver = None
            try:
                self.wait(lambda: self.port_ready(port), "Broker未启动")
                base_master, base_slave = pty.openpty()
                px4_master, px4_slave = pty.openpty()
                box_master, box_slave = pty.openpty()
                descriptors = [base_master, base_slave, px4_master, px4_slave, box_master, box_slave]
                tty.setraw(base_slave)
                tty.setraw(px4_slave)
                tty.setraw(box_slave)
                config = json.loads((ROOT / "config/config.example.json").read_text())
                config["serial"] = {"device": os.ttyname(box_slave), "baud": 115200}
                config["rtk_base"] = {"enabled": True, "device": os.ttyname(base_slave), "baud": 115200,
                    "px4_device": os.ttyname(px4_slave), "px4_baud": 57600,
                    "survey_min_duration_s": 60, "survey_accuracy_m": 2.0, "stale_after_ms": 1000,
                    "status_file": str(folder / "status.json")}
                config["mqtt"]["connection"].update(host="127.0.0.1", port=port)
                config["mqtt"]["auth"] = {"username": "", "password": ""}
                config["mqtt"]["topics"]["namespace"] = "rtkindoor"
                config["identity"]["binding_file"] = str(folder / "box-binding.json")
                config["logging"]["file"] = ""
                path = folder / "config.json"
                path.write_text(json.dumps(config))
                subscriber = subprocess.Popen(["mosquitto_sub", "-h", "127.0.0.1", "-p", str(port),
                    "-t", "rtkindoor/#", "-v"], stdout=subprocess.PIPE, text=True)
                processes.append(subscriber)
                messages = queue.Queue()
                threading.Thread(target=lambda: [messages.put(line) for line in subscriber.stdout], daemon=True).start()
                receiver = Receiver(base_master)
                px4_output = bytearray()
                gps_enabled = threading.Event()
                gps_enabled.set()
                def stream(master, fixture, stop):
                    while not stop.wait(0.25):
                        os.write(master, fixture)
                        if master == px4_master and gps_enabled.is_set():
                            os.write(master, subprocess.check_output([FIXTURE, "gps", str(time.monotonic_ns() // 1000)]))
                        if select.select([master], [], [], 0)[0]:
                            outgoing = os.read(master, 4096)
                            if master == px4_master: px4_output.extend(outgoing)
                for master, fixture, stop in [
                        (box_master, subprocess.check_output([FIXTURE, "box"]), stop_box),
                        (px4_master, subprocess.check_output([FIXTURE]), stop_px4)]:
                    thread = threading.Thread(target=stream, args=(master, fixture, stop), daemon=True)
                    thread.start()
                    streamers.append(thread)
                app_log = (folder / "app.log").open("w+")
                logs.append(app_log)
                app = subprocess.Popen([BINARY, str(path), "--config-writer=disabled"], stdout=app_log, stderr=app_log)
                processes.append(app)
                def status():
                    try:
                        return json.loads((folder / "status.json").read_text())
                    except (FileNotFoundError, json.JSONDecodeError):
                        return {}
                received = []
                def message(predicate, after=0):
                    while not messages.empty():
                        line = messages.get_nowait()
                        topic, payload = line.rstrip().split(" ", 1)
                        try: received.append((topic, json.loads(payload)))
                        except json.JSONDecodeError: pass
                    return next((item for item in received[after:] if predicate(*item)), None)
                self.wait(lambda: message(lambda t, p: t == f"{BOX_TOPIC}/registration" and
                    p.get("status") == "online" and p.get("device_id") == BOX_ID and p.get("device_type") == "cns_box"),
                    "没有用F407箱子身份注册唯一cns_box", app, app_log)
                self.wait(lambda: status().get("px4", {}).get("identity_verified"),
                          "RTK附属端口未取得真实格式的模拟PX4身份", app, app_log)
                self.wait(lambda: status().get("base", {}).get("receiver_verified"), "未读到模拟基站真实协议响应", app, app_log)
                self.wait(lambda: status().get("px4", {}).get("gps_observation", {}).get("known"),
                          "模拟PX4没有逐样本GPS观测", app, app_log)
                observed = lambda t, p: (
                    "/telemetry/" in t and
                    p.get("telemetry", {}).get("heartbeat", {}).get("type") == 18 and
                    p.get("telemetry", {}).get("rtk", {}).get("box_identity_verified") is True and
                    p.get("telemetry", {}).get("rtk", {}).get("base", {}).get("receiver_verified") and
                    p.get("telemetry", {}).get("rtk", {}).get("px4", {}).get("identity_verified"))
                self.wait(lambda: message(observed), "没有在箱子Topic上传基站与PX4附属遥测", app, app_log)
                cloud = message(observed)[1]
                self.assertEqual(cloud["device_id"], BOX_ID)
                self.assertEqual(cloud["telemetry"]["heartbeat"]["type"], 18)
                self.assertEqual(cloud["telemetry"]["heartbeat"]["autopilot"], 8)
                self.assertEqual(cloud["telemetry"]["rtk"]["px4"]["device_id"], PX4_ID)
                self.assertEqual(cloud["telemetry"]["rtk"]["px4"]["telemetry"]["heartbeat"]["autopilot"], 12)
                self.assertFalse(status()["base"]["survey_in"]["valid"])
                self.assertEqual(json.loads((folder / "box-binding.json").read_text()),
                    {"schema_version": 1, "device_id": BOX_ID, "device_type": "cns_box"})
                request = {"schema_version": 1, "command_id": "00000000-0000-4000-8000-000000000001",
                    "request_id": "00000000-0000-4000-8000-000000000003", "session_id": "00000000-0000-4000-8000-000000000002",
                    "action_id": "00000000-0000-4000-8000-000000000003", "target": {"device_id": BOX_ID},
                    "lease_version": 1, "operation": "rtk_base_survey_in", "expires_at": "2099-01-01T00:00:00.000Z",
                    "parameters": {"minimum_duration_s": 60, "accuracy_limit_m": 2.0}}
                def send():
                    subprocess.run(["mosquitto_pub", "-h", "127.0.0.1", "-p", str(port), "-q", "2",
                        "-t", f"{BOX_TOPIC}/experiment/set", "-m", json.dumps(request)], check=True, timeout=5)
                send()
                completed = lambda t, p: t.endswith("/experiment/ack") and p.get("command_id") == request["command_id"] and p.get("status") == "completed"
                self.wait(lambda: message(completed), "配置没有通过真实ACK与读回完成", app, app_log)
                self.assertEqual(message(completed)[1]["fact"]["base"]["configuration"]["storage"], "RAM")
                self.assertEqual(set(receiver.configuration_layers), {1})
                self.assertEqual(struct.unpack("<I", receiver.values[0x40030010])[0], 60)
                self.assertEqual(struct.unpack("<I", receiver.values[0x40030011])[0], 20000)
                self.assertEqual(receiver.survey_starts, 1)
                request["command_id"] = "00000000-0000-4000-8000-000000000006"
                send()
                self.wait(lambda: message(lambda t, p: t.endswith("/experiment/ack") and
                    p.get("command_id") == request["command_id"] and p.get("status") == "completed"),
                    "新投递没有返回同一动作的结果")
                self.assertEqual(receiver.survey_starts, 1)
                self.wait(lambda: receiver.navigation_mode == 1,
                    "模拟接收机没有进入观测导航周期", app, app_log)
                request["command_id"] = "00000000-0000-4000-8000-000000000008"
                request["request_id"] = request["action_id"] = "00000000-0000-4000-8000-000000000007"
                send()
                self.wait(lambda: message(lambda t, p: t.endswith("/experiment/ack") and
                    p.get("command_id") == request["command_id"] and p.get("status") == "completed"),
                    "重新开始观测没有完成配置核验", app, app_log)
                self.assertFalse(receiver.enabled_before_stop, "导航周期尚未停用，驱动就重新启用了Survey-in")
                self.assertEqual(receiver.survey_starts, 2)
                receiver.disable_ack_delay_s = 0.8
                request["command_id"] = "00000000-0000-4000-8000-000000000004"
                request["request_id"] = request["action_id"] = "00000000-0000-4000-8000-000000000005"
                request["expires_at"] = (datetime.now(timezone.utc) + timedelta(seconds=0.4)).isoformat(timespec="milliseconds").replace("+00:00", "Z")
                send()
                self.wait(lambda: message(lambda t, p: t.endswith("/experiment/ack") and
                    p.get("command_id") == request["command_id"] and p.get("error_code") == "configuration_timeout"),
                    "进行中的配置过期后没有报告失败", app, app_log)
                self.wait(receiver.delayed_ack_sent.is_set, "模拟接收机没有发送迟到ACK", app, app_log)
                time.sleep(0.2)
                self.assertEqual(receiver.survey_starts, 2, "过期动作的迟到ACK仍启动了新的Survey-in")
                stop_px4.set()
                self.wait(lambda: status().get("px4", {}).get("identity_verified") is False, "飞控静默后仍沿用实时身份", timeout=9)
                message(lambda t, p: False)
                after_px4_loss = len(received)
                self.wait(lambda: message(lambda t, p: "/telemetry/" in t and
                    p.get("telemetry", {}).get("rtk", {}).get("px4", {}).get("identity_verified") is False,
                    after=after_px4_loss), "PX4静默后箱子没有继续上传附属链路失联状态", app, app_log)
                request["command_id"] = "00000000-0000-4000-8000-000000000009"
                request["request_id"] = request["action_id"] = "00000000-0000-4000-8000-000000000010"
                request["operation"] = "rtk_base_status"
                request["expires_at"] = "2099-01-01T00:00:00.000Z"
                request.pop("parameters")
                send()
                self.wait(lambda: message(completed), "PX4失联阻断了箱子身份下的基站状态动作", app, app_log)
                self.assertTrue(message(completed)[1]["fact"]["base"]["receiver_verified"])
                self.assertFalse(message(completed)[1]["fact"]["base"]["protocol"]["stale"])
                receiver.stop.set()
                self.wait(lambda: status().get("base", {}).get("protocol", {}).get("stale") is True, "基站静默未标过期")
                message(lambda t, p: False)
                registrations = [(topic, payload) for topic, payload in received if topic.endswith("/registration")]
                self.assertEqual(len(registrations), 1, "PX4附属身份或失联触发了额外注册")
                self.assertEqual(registrations[0][0], f"{BOX_TOPIC}/registration")
                self.assertEqual(registrations[0][1]["device_type"], "cns_box")
                self.assertEqual(registrations[0][1]["status"], "online")
                self.assertEqual({topic.split("/")[1] for topic, _ in received}, {BOX_ID})

                # 重启后不再提供F407帧，已落盘的箱子身份仍可承载独立RTK实验。
                stop_box.set()
                for thread in streamers: thread.join(timeout=1)
                app.terminate()
                app.wait(timeout=5)
                termios.tcflush(box_slave, termios.TCIFLUSH)
                self.wait(lambda: message(lambda t, p: t.endswith("/registration") and
                    p.get("status") == "offline"), "第一进程退出后没有箱子离线注册")
                receiver.thread.join(timeout=1)
                receiver = Receiver(base_master)
                stop_px4.clear()
                thread = threading.Thread(target=stream,
                    args=(px4_master, subprocess.check_output([FIXTURE]), stop_px4), daemon=True)
                thread.start()
                streamers.append(thread)
                (folder / "status.json").unlink(missing_ok=True)
                cached_begin = len(received)
                app_log = (folder / "cached-box-app.log").open("w+")
                logs.append(app_log)
                app = subprocess.Popen([BINARY, str(path), "--config-writer=disabled"], stdout=app_log, stderr=app_log)
                processes.append(app)
                self.wait(lambda: message(lambda t, p: t == f"{BOX_TOPIC}/registration" and
                    p.get("status") == "online" and p.get("device_type") == "cns_box",
                    after=cached_begin), "已持久化箱子身份未能在F407缺席时注册", app, app_log)
                self.wait(lambda: message(lambda t, p: t.endswith("/runtime/status/v1") and
                    p.get("device_id") == BOX_ID and p.get("online") == "0",
                    after=cached_begin), "F407缺席时没有继续发布常规在线状态", app, app_log)
                cached_status = message(lambda t, p: t.endswith("/runtime/status/v1"),
                                        after=cached_begin)[1]
                self.assertEqual(set(cached_status), {"device_id", "school_id", "dcdw_label",
                    "model_version", "device_name", "device_type", "capabilities", "product",
                    "online", "device_status"})
                cached_observation = lambda t, p: (
                    "/telemetry/" in t and
                    p.get("telemetry", {}).get("rtk", {}).get("box_identity_verified") is False and
                    p.get("telemetry", {}).get("rtk", {}).get("base", {}).get("receiver_verified") and
                    p.get("telemetry", {}).get("rtk", {}).get("px4", {}).get("identity_verified"))
                self.wait(lambda: message(cached_observation, after=cached_begin),
                    "F407缺席时RTK没有继续在缓存箱子身份下上传", app, app_log)
                request["command_id"] = "00000000-0000-4000-8000-000000000020"
                request["request_id"] = request["action_id"] = "00000000-0000-4000-8000-000000000021"
                request["operation"] = "rtk_base_survey_in"
                request["parameters"] = {"minimum_duration_s": 90, "accuracy_limit_m": 1.5}
                send()
                terminal = lambda t, p: t.endswith("/experiment/ack") and p.get("command_id") == request["command_id"] and p.get("status") in {"completed", "rejected", "failed"}
                self.wait(lambda: message(terminal, after=cached_begin),
                    "F407缺席时缓存箱子身份未收到Survey-in终态ACK", app, app_log)
                self.assertEqual(message(terminal, after=cached_begin)[1]["status"], "completed",
                    json.dumps(message(terminal, after=cached_begin)[1], ensure_ascii=False))
                self.assertEqual(receiver.survey_starts, 1)
                self.assertEqual(set(receiver.configuration_layers), {1})
                self.assertEqual(struct.unpack("<I", receiver.values[0x40030010])[0], 90)
                self.assertEqual(struct.unpack("<I", receiver.values[0x40030011])[0], 15000)
                request["command_id"] = "00000000-0000-4000-8000-000000000022"
                request["request_id"] = request["action_id"] = "00000000-0000-4000-8000-000000000023"
                request["operation"] = "rtk_base_status"
                request.pop("parameters")
                send()
                self.wait(lambda: message(completed, after=cached_begin),
                    "F407缺席时缓存箱子身份不能查询基站状态", app, app_log)
                message(lambda t, p: False)
                cached_messages = received[cached_begin:]
                cached_telemetry = [(t, p) for t, p in cached_messages if "/telemetry/" in t]
                self.assertTrue(cached_telemetry)
                self.assertTrue(any(t.endswith("/telemetry/snapshot/v1") for t, _ in cached_telemetry))
                for topic, sample in cached_telemetry:
                    self.assertEqual(sample["device_id"], BOX_ID)
                    self.assertFalse(sample["telemetry"]["rtk"]["box_identity_verified"])
                    self.assertLessEqual(set(sample["telemetry"]), {"rtk", "cellular_5g"})
                    self.assertFalse(set(sample) & {"modules", "alarms", "logs", "dcdw_label"})
                    if topic.endswith("/telemetry/snapshot/v1"):
                        self.assertEqual(sample.get("drone_id"), {"basic_id": {}},
                            "缓存RTK快照必须满足服务器schema3的空DroneID对象合同")
                    else:
                        self.assertNotIn("drone_id", sample)
                cached_registrations = [(t, p) for t, p in cached_messages if t.endswith("/registration")]
                self.assertEqual(len(cached_registrations), 1)
                self.assertEqual(cached_registrations[0][0], f"{BOX_TOPIC}/registration")
                self.assertEqual(cached_registrations[0][1]["device_id"], BOX_ID)
                self.assertEqual(cached_registrations[0][1]["status"], "online")
                self.assertEqual({topic.split("/")[1] for topic, _ in cached_messages}, {BOX_ID})

                # 缓存箱子身份运行移动站，核对实际串口原帧与MQTT观察，绝不虚构接收ACK。
                rover_counter = 100
                def rover(operation):
                    nonlocal rover_counter
                    rover_counter += 1
                    identifier = f"00000000-0000-4000-8000-{rover_counter:012d}"
                    request.update(command_id=identifier, request_id=identifier, action_id=identifier,
                                   operation=operation, expires_at="2099-01-01T00:00:00.000Z")
                    request.pop("parameters", None)
                    self.assertEqual(len(request), 9)
                    send()
                    ack = lambda t, p: t.endswith("/experiment/ack") and p.get("command_id") == identifier and p.get("status") in {"completed", "failed", "rejected"}
                    self.wait(lambda: message(ack), f"{operation}没有终态ACK", app, app_log)
                    response = message(ack)[1]
                    self.assertEqual(response["status"], "completed", json.dumps(response))
                    return response["fact"]["rtk"]
                def decoded():
                    output = subprocess.check_output([FIXTURE, "decode-rtcm"], input=bytes(px4_output)).decode()
                    return [line.split() for line in output.splitlines()]
                def rtcm_packets():
                    return [item for item in decoded() if item[0] == "233"]
                self.assertEqual(status()["relay"]["state"], "stopped")
                started = rover("rtk_rover_start")
                self.assertEqual(started["relay"]["state"], "waiting_base")
                self.assertNotEqual(started["relay"]["state"], "Fixed")
                receiver.survey_complete = True
                self.wait(lambda: status().get("base", {}).get("survey_in", {}).get("completed"),
                          "基站未接受完成SVIN样本", app, app_log)
                # CRC24Q合法的封装样本，不宣称它是完整业务1005报文。
                rtcm = bytes.fromhex("d300023ed0a4e000")
                before = len(rtcm_packets())
                os.write(base_master, rtcm[:-1] + bytes([rtcm[-1] ^ 1]))
                time.sleep(0.35)
                self.assertEqual(len(rtcm_packets()), before, "坏CRC样本被转发")
                os.write(base_master, rtcm)
                self.wait(lambda: len(rtcm_packets()) > before, "合法RTCM没有实际写入3DR PTY", app, app_log)
                self.assertEqual(bytes.fromhex(rtcm_packets()[-1][2]), rtcm)
                self.wait(lambda: status().get("observation", {}).get("latest_transfer"), "状态未保留实际发送样本")
                observation = status()["observation"]
                self.assertEqual(observation["latest_rtcm"]["raw_hex"], rtcm.hex())
                transfer = observation["latest_transfer"]
                self.assertTrue(transfer["fragments"])
                for fragment in transfer["fragments"]:
                    parsed = subprocess.check_output([FIXTURE, "decode-rtcm"], input=bytes.fromhex(fragment["wire_hex"])).decode().split()
                    self.assertEqual(parsed[0], "233")
                    self.assertEqual(bytes.fromhex(parsed[2]), rtcm)
                snapshot_begin = len(received)
                self.wait(lambda: message(lambda t, p: t.endswith("/telemetry/snapshot/v1") and
                    p.get("telemetry", {}).get("rtk", {}).get("observation", {}).get("latest_transfer"), after=snapshot_begin),
                    "云端snapshot缺少实际协议样本", app, app_log, timeout=14)
                snapshot = message(lambda t, p: t.endswith("/telemetry/snapshot/v1") and
                    p.get("telemetry", {}).get("rtk", {}).get("observation", {}).get("latest_transfer"), after=snapshot_begin)[1]["telemetry"]["rtk"]
                self.assertEqual(snapshot["observation"]["latest_rtcm"]["raw_hex"], rtcm.hex())
                self.assertIn("raw_hex", snapshot["px4"]["gps_observation"])
                self.assertIn("transitions", snapshot["px4"]["gps_observation"])
                self.assertGreater(snapshot["px4"]["gps_observation"]["sample_sequence"], 1)
                self.assertEqual(rover("rtk_rover_pause")["relay"]["state"], "paused")
                before = len(rtcm_packets())
                before_heartbeat = sum(item[0] == "0" for item in decoded())
                before_gps = status()["px4"]["gps_observation"]["sample_sequence"]
                os.write(base_master, rtcm)
                time.sleep(1.2)
                self.assertEqual(len(rtcm_packets()), before)
                self.assertGreater(sum(item[0] == "0" for item in decoded()), before_heartbeat)
                self.assertGreater(status()["px4"]["gps_observation"]["sample_sequence"], before_gps)
                rover("rtk_rover_resume")
                time.sleep(0.35)
                self.assertEqual(len(rtcm_packets()), before, "恢复重放了暂停期间样本")
                os.write(base_master, rtcm)
                self.wait(lambda: len(rtcm_packets()) == before + 1, "恢复后新RTCM未转发")
                self.assertEqual(rover("rtk_rover_status")["relay"]["forwarded_frames"], 2)
                self.assertEqual(rover("rtk_rover_stop")["relay"]["state"], "stopped")
                before = len(rtcm_packets())
                os.write(base_master, rtcm)
                time.sleep(0.35)
                self.assertEqual(len(rtcm_packets()), before)
                gps_enabled.clear()
                self.wait(lambda: status().get("px4", {}).get("gps_observation", {}).get("stale"),
                          "心跳继续但GPS冻结时没有过期", app, app_log)
                self.assertTrue(status()["px4"]["identity_verified"])
                self.assertIsNone(status()["px4"]["gps_observation"]["fix_type"])
                realtime_begin = len(received)
                self.wait(lambda: message(lambda t, p: t.endswith("/telemetry/realtime/v1") and
                    p.get("telemetry", {}).get("rtk", {}).get("observation", {}).get("samples_included") is False,
                    after=realtime_begin), "高频遥测没有紧凑观察", app, app_log)
                compact = message(lambda t, p: t.endswith("/telemetry/realtime/v1") and
                    p.get("telemetry", {}).get("rtk", {}).get("observation", {}).get("samples_included") is False,
                    after=realtime_begin)[1]["telemetry"]["rtk"]
                self.assertNotIn("raw_hex", json.dumps(compact))
                self.assertNotIn("wire_hex", json.dumps(compact))
                self.assertNotIn("transitions", compact["px4"]["gps_observation"])
                gps_enabled.set()

                # 真实格式的其他箱子到来时，缓存身份不得继续发布RTK或静默改绑。
                stop_box.clear()
                other_box_thread = threading.Thread(target=stream,
                    args=(box_master, subprocess.check_output([FIXTURE, "otherbox"]), stop_box), daemon=True)
                other_box_thread.start()
                streamers.append(other_box_thread)
                conflict_begin = len(received)
                self.wait(lambda: "F407身份与持久化绑定冲突" in Path(app_log.name).read_text(),
                    "不同F407身份出现后未检测到冲突", app, app_log)
                request["command_id"] = "00000000-0000-4000-8000-000000000030"
                request["request_id"] = request["action_id"] = "00000000-0000-4000-8000-000000000031"
                request["operation"] = "inspect_pi_link"
                request.pop("parameters", None)
                send()
                conflict_ack = lambda t, p: t.endswith("/experiment/ack") and p.get("command_id") == request["command_id"]
                self.wait(lambda: message(conflict_ack, after=conflict_begin),
                    "身份冲突后的自检未返回拒绝回执", app, app_log)
                rejected = message(conflict_ack, after=conflict_begin)[1]
                self.assertEqual(rejected["status"], "rejected")
                self.assertEqual(rejected["error_code"], "identity_unavailable")
                message(lambda t, p: False)
                after_conflict = len(received)
                first_sample = status()["base"]["survey_in"]["sample_sequence"]
                self.wait(lambda: status().get("base", {}).get("survey_in", {}).get("sample_sequence", 0) >= first_sample + 2,
                    "身份冲突后本地基站未继续采集", app, app_log)
                message(lambda t, p: False)
                self.assertFalse([(t, p) for t, p in received[after_conflict:]
                    if "/telemetry/" in t and "rtk" in p.get("telemetry", {})],
                    "身份冲突后仍继续上传RTK帧")
                self.assertEqual(json.loads((folder / "box-binding.json").read_text()),
                    {"schema_version": 1, "device_id": BOX_ID, "device_type": "cns_box"})
                for topic, payload in received[conflict_begin:]:
                    self.assertTrue(topic.startswith(f"{BOX_TOPIC}/"))
                    if topic.endswith("/registration"):
                        self.assertEqual(payload["device_id"], BOX_ID)
                        self.assertEqual(payload["device_type"], "cns_box")
                stop_box.set()
                other_box_thread.join(timeout=1)

                # 同样的PX4与基站在未绑定箱子上只保留本地观测，不能借用PX4身份上云。
                app.terminate()
                app.wait(timeout=5)
                termios.tcflush(box_slave, termios.TCIFLUSH)
                self.wait(lambda: message(lambda t, p: t.endswith("/registration") and
                    p.get("status") == "offline", after=cached_begin), "缓存身份进程退出后没有离线注册")
                config["identity"]["binding_file"] = str(folder / "unbound-box.json")
                path.write_text(json.dumps(config))
                (folder / "status.json").unlink(missing_ok=True)
                unbound_begin = len(received)
                app_log = (folder / "unbound-box-app.log").open("w+")
                logs.append(app_log)
                app = subprocess.Popen([BINARY, str(path), "--config-writer=disabled"], stdout=app_log, stderr=app_log)
                processes.append(app)
                self.wait(lambda: status().get("px4", {}).get("identity_verified") and
                    status().get("base", {}).get("receiver_verified"),
                    "未绑定场景的本地PX4与基站未继续工作", app, app_log)
                first_sample = status()["base"]["survey_in"]["sample_sequence"]
                self.wait(lambda: status().get("base", {}).get("survey_in", {}).get("sample_sequence", 0) >= first_sample + 2,
                    "未绑定场景没有继续采集新的基站历元", app, app_log)
                message(lambda t, p: False)
                self.assertFalse(received[unbound_begin:], "未绑定箱子仍借用附属设备身份向云端发布")
                self.assertFalse((folder / "unbound-box.json").exists())
            finally:
                stop_box.set()
                stop_px4.set()
                if receiver: receiver.stop.set()
                for thread in streamers: thread.join(timeout=1)
                if receiver: receiver.thread.join(timeout=1)
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                        try: process.wait(timeout=5)
                        except subprocess.TimeoutExpired: process.kill(); process.wait()
                    if process.stdout: process.stdout.close()
                for descriptor in descriptors: os.close(descriptor)
                for log in logs: log.close()

    def wait(self, predicate, failure, process=None, log=None, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate(): return
            if process and process.poll() is not None: break
            time.sleep(0.04)
        if log:
            log.flush(); log.seek(0)
            failure += "\n" + log.read()[-5000:]
        self.fail(failure)

    @staticmethod
    def port_ready(port):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1): return True
        except OSError: return False


if __name__ == "__main__":
    unittest.main()
