"""Real gateway process on a PTY and local broker; this is simulated link evidence."""
import base64
import json
import os
from pathlib import Path
import pty
import queue
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import tty
import unittest
import uuid

BINARY, FIXTURE = sys.argv[1:3]
del sys.argv[1:3]
ROOT = Path(__file__).resolve().parents[1]
BOX_ID = "DCDWCNS1GNSSTEST0001"
TOPIC = f"gnsstest/{BOX_ID}"
RAW_TOPIC = f"{TOPIC}/gnss/raw/v1"
GOOD = b"$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"
BAD = b"$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00\r\n"
UBX = bytes([0xB5, 0x62, 1, 7, 2, 0, 217, 29, 0, 0])


class GatewayTest(unittest.TestCase):
    def test_capture_raw_bytes_stop_duration_and_serial_loss(self):
        if any(not shutil.which(name) for name in
               ["mosquitto", "mosquitto_sub", "mosquitto_pub"]):
            self.skipTest("requires local mosquitto and mosquitto-clients")
        with tempfile.TemporaryDirectory(prefix="cns-gnss-gateway-") as directory:
            folder = Path(directory)
            with socket.socket() as unused:
                unused.bind(("127.0.0.1", 0))
                port = unused.getsockname()[1]
            broker_config = folder / "broker.conf"
            broker_config.write_text(
                f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
            broker_log = (folder / "broker.log").open("w+")
            app_log = (folder / "app.log").open("w+")
            broker = subprocess.Popen(["mosquitto", "-c", str(broker_config)],
                                      stdout=broker_log, stderr=broker_log)
            processes = [broker]
            descriptors = []
            stopped = threading.Event()
            streamer = None
            stream_errors = []
            received = []
            incoming = queue.Queue()
            app = None
            try:
                self.wait(lambda: self.port_ready(port), "broker did not start", timeout=3)
                master, slave = pty.openpty()
                descriptors = [master, slave]
                tty.setraw(slave)
                config = json.loads((ROOT / "config/config.example.json").read_text())
                config["serial"] = {"device": os.ttyname(slave), "baud": 115200}
                config["gnss"] = {"enabled": True,
                                  "archive_directory": str(folder / "gnss")}
                config["rtk_base"]["enabled"] = False
                config["mqtt"]["connection"].update(host="127.0.0.1", port=port,
                                                        client_id="gnss-gateway-test")
                config["mqtt"]["auth"] = {"username": "", "password": ""}
                config["mqtt"]["topics"]["namespace"] = "gnsstest"
                config["identity"]["binding_file"] = str(folder / "binding.json")
                config["logging"]["file"] = ""
                path = folder / "config.json"
                path.write_text(json.dumps(config))
                subscriber = subprocess.Popen([
                    "mosquitto_sub", "-h", "127.0.0.1", "-p", str(port), "-q", "1",
                    "-t", "gnsstest/#", "-F", "%q %r %t %p"],
                    stdout=subprocess.PIPE, text=True)
                processes.append(subscriber)
                def read_messages():
                    for line in subscriber.stdout:
                        incoming.put(line)
                reader = threading.Thread(target=read_messages, daemon=True)
                reader.start()
                def stream():
                    tick = 0
                    while not stopped.wait(0.2):
                        try:
                            tick += 1
                            wire = subprocess.check_output([FIXTURE, str(tick)], timeout=2)
                            os.write(master, wire)
                            if select.select([master], [], [], 0)[0]:
                                os.read(master, 4096)
                        except (OSError, subprocess.SubprocessError) as error:
                            stream_errors.append(str(error))
                            return
                streamer = threading.Thread(target=stream, daemon=True)
                streamer.start()
                app = subprocess.Popen([BINARY, str(path), "--config-writer=disabled"],
                                       stdout=app_log, stderr=app_log)
                processes.append(app)

                def drain():
                    while not incoming.empty():
                        line = incoming.get_nowait()
                        qos, retain, topic, payload = line.rstrip("\n").split(" ", 3)
                        try:
                            received.append({"topic": topic, "payload": json.loads(payload),
                                             "qos": int(qos), "retain": int(retain),
                                             "bytes": len(payload.encode())})
                        except json.JSONDecodeError:
                            self.fail(f"invalid JSON on {topic}: {payload}")

                def message(predicate, after=0):
                    drain()
                    return next((item for item in received[after:] if predicate(item)), None)

                def gps(after=0, newer=0):
                    return message(lambda item:
                        "/telemetry/" in item["topic"] and
                        item["payload"].get("telemetry", {}).get("gps", {})
                        .get("time_usec", 0) > newer, after)

                self.wait(lambda: message(lambda item:
                    item["topic"] == f"{TOPIC}/registration" and
                    item["payload"].get("status") == "online"),
                    "F407 fixture did not register", app, app_log)
                self.wait(lambda: gps(), "ordinary GPS missing before capture", app, app_log)
                before_gps = gps()["payload"]["telemetry"]["gps"]["time_usec"]
                self.assertFalse(message(lambda item: item["topic"] == RAW_TOPIC),
                                 "raw was published before explicit capture")
                session_id, capture_id = str(uuid.uuid4()), str(uuid.uuid4())

                def action(operation, parameters=None, session=session_id):
                    action_id, command_id = str(uuid.uuid4()), str(uuid.uuid4())
                    request = {"schema_version": 1, "command_id": command_id,
                        "request_id": action_id, "session_id": session,
                        "action_id": action_id, "target": {"device_id": BOX_ID},
                        "lease_version": 1, "operation": operation,
                        "expires_at": "2099-01-01T00:00:00.000Z"}
                    if parameters is not None:
                        request["parameters"] = parameters
                    subprocess.run(["mosquitto_pub", "-h", "127.0.0.1", "-p", str(port),
                        "-q", "1", "-t", f"{TOPIC}/experiment/set", "-m", json.dumps(request)],
                        check=True, timeout=2)
                    wanted = lambda item: item["topic"] == f"{TOPIC}/experiment/ack" and \
                        item["payload"].get("command_id") == command_id
                    self.wait(lambda: message(wanted), f"no ACK for {operation}", app, app_log,
                              timeout=4)
                    ack = message(wanted)["payload"]
                    self.assertEqual(ack["action_id"], action_id)
                    return ack

                start = action("gnss_capture_start", {"capture_id": capture_id,
                                                      "duration_seconds": 10})
                self.assertEqual(start["status"], "completed")
                self.assertEqual(start["fact"]["gnss"]["capture_id"], capture_id)

                def frames():
                    drain()
                    return [frame for item in received if item["topic"] == RAW_TOPIC
                            and item["payload"]["capture_id"] == capture_id
                            for frame in item["payload"]["frames"]]

                def all_wire_types():
                    return {base64.b64decode(frame["raw_base64"], validate=True)
                            for frame in frames()} >= {GOOD, BAD, UBX}

                self.wait(all_wire_types, "raw batch lost NMEA/BAD/UBX original bytes", app, app_log)
                self.wait(lambda: gps(newer=before_gps),
                          "ordinary GPS stopped during capture", app, app_log)
                for frame in frames():
                    wire = base64.b64decode(frame["raw_base64"], validate=True)
                    self.assertEqual(frame["raw_length"], len(wire))
                    self.assertEqual(frame["source"]["system_id"], 42)
                    self.assertEqual(frame["source"]["component_id"], 193)
                    self.assertEqual(frame["source_checksum_valid"], wire != BAD)
                    self.assertEqual(frame["protocol"], "ubx" if wire == UBX else "nmea")
                rejected = action("gnss_capture_stop", {"capture_id": str(uuid.uuid4())})
                self.assertEqual(rejected["status"], "rejected")
                stop = action("gnss_capture_stop", {"capture_id": capture_id})
                self.assertEqual(stop["status"], "completed")
                self.assertEqual(stop["fact"]["gnss"]["end_reason"], "capture_stopped")
                time.sleep(0.4)
                drain()
                stopped_after = len(received)
                latest_gps = max(item["payload"].get("telemetry", {}).get("gps", {})
                                 .get("time_usec", 0) for item in received)
                self.wait(lambda: gps(after=stopped_after, newer=latest_gps),
                          "ordinary GPS stopped after capture", app, app_log)
                time.sleep(0.3)
                drain()
                self.assertFalse([item for item in received[stopped_after:]
                                  if item["topic"] == RAW_TOPIC], "raw continued after stop")
                late = subprocess.run(["mosquitto_sub", "-h", "127.0.0.1", "-p", str(port),
                    "-t", RAW_TOPIC, "-C", "1", "-W", "1"],
                    capture_output=True, text=True, timeout=2)
                self.assertFalse(late.stdout, "raw batch was retained by broker")
                self.assertNotEqual(late.returncode, 0)

                capture_id = str(uuid.uuid4())
                duration_start = len(received)
                self.assertEqual(action("gnss_capture_start", {"capture_id": capture_id,
                    "duration_seconds": 1})["status"], "completed")
                self.wait(all_wire_types, "duration capture produced no complete frames", app, app_log)
                time.sleep(1.1)
                status = action("gnss_status")["fact"]["gnss"]
                self.assertEqual(status["end_reason"], "duration_elapsed")
                self.assertGreater(status["frames"], 0)
                self.wait(lambda: gps(after=duration_start),
                          "GPS absent around duration completion", app, app_log)
                self.assertTrue((folder / "gnss/current.jsonl").is_file())
                self.assertTrue((folder / "gnss/previous.jsonl").is_file())

                capture_id = str(uuid.uuid4())
                self.assertEqual(action("gnss_capture_start", {"capture_id": capture_id,
                    "duration_seconds": 10})["status"], "completed")
                self.wait(all_wire_types, "network-loss capture produced no raw", app, app_log)
                broker.terminate()
                broker.wait(timeout=3)
                time.sleep(0.3)
                drain()
                reconnected_after = len(received)
                broker = subprocess.Popen(["mosquitto", "-c", str(broker_config)],
                                          stdout=broker_log, stderr=broker_log)
                processes.append(broker)
                self.wait(lambda: message(lambda item:
                    item["topic"] == f"{TOPIC}/registration" and
                    item["payload"].get("status") == "online", reconnected_after),
                    "gateway did not reconnect to broker", app, app_log, timeout=8)
                network = action("gnss_status")["fact"]["gnss"]
                self.assertEqual(network["state"], "failed")
                self.assertEqual(network["end_reason"], "mqtt_disconnected")
                self.wait(lambda: gps(after=reconnected_after),
                          "ordinary GPS missing after broker reconnect", app, app_log)

                capture_id = str(uuid.uuid4())
                self.assertEqual(action("gnss_capture_start", {"capture_id": capture_id,
                    "duration_seconds": 10})["status"], "completed")
                stopped.set()
                streamer.join(timeout=2)
                self.assertFalse(stream_errors, stream_errors)
                os.close(master)
                descriptors.remove(master)
                time.sleep(0.4)
                failed = action("gnss_status")["fact"]["gnss"]
                self.assertEqual(failed["state"], "failed")
                self.assertEqual(failed["end_reason"], "serial_disconnected")
                drain()
                for item in received:
                    if item["topic"] == RAW_TOPIC:
                        self.assertEqual(item["qos"], 1)
                        self.assertEqual(item["retain"], 0)
                        self.assertLessEqual(item["bytes"], 16384)
                        self.assertLessEqual(len(item["payload"]["frames"]), 32)
                    elif "/telemetry/" in item["topic"]:
                        self.assertNotIn("raw_base64", json.dumps(item["payload"]))
                self.assertIsNone(app.poll(), "gateway exited unexpectedly")
            finally:
                stopped.set()
                if streamer:
                    streamer.join(timeout=2)
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                    if process.stdout:
                        process.stdout.close()
                for descriptor in descriptors:
                    os.close(descriptor)
                app_log.close()
                broker_log.close()

    def wait(self, predicate, failure, process=None, log=None, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            if process and process.poll() is not None:
                break
            time.sleep(0.03)
        if log:
            log.flush()
            log.seek(0)
            failure += "\n" + log.read()[-5000:]
        self.fail(failure)

    @staticmethod
    def port_ready(port):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return True
        except OSError:
            return False


if __name__ == "__main__":
    unittest.main()
