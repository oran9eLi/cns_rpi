"""Actual gateway + sole PTY reader + isolated MQTT; synthetic F407 evidence only."""
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

BINARY, FIXTURE = sys.argv[1:3]
del sys.argv[1:3]
ROOT = Path(__file__).resolve().parents[1]
BOX_ID = "DCDWCNS1S2MEAG1VTA0C"
PREFIX = f"imutest/{BOX_ID}"
SAMPLE = f"{PREFIX}/imu/sample/v1"
REALTIME = f"{PREFIX}/telemetry/realtime/v1"
SNAPSHOT = f"{PREFIX}/telemetry/snapshot/v1"


class ImuGatewayTest(unittest.TestCase):
    def test_contract_through_real_process(self):
        self.run_gateway(telemetry=True)

    def test_raw_topic_independent_of_telemetry_switches(self):
        self.run_gateway(telemetry=False)

    def test_cached_identity_cannot_authorize_imu(self):
        self.run_gateway(telemetry=False, cached_identity=True)

    def run_gateway(self, telemetry, cached_identity=False):
        if any(not shutil.which(name) for name in ["mosquitto", "mosquitto_sub"]):
            self.skipTest("requires mosquitto and mosquitto-clients")
        contract = json.loads((ROOT / "tests/fixtures/bmi088_contract.json").read_text())
        with tempfile.TemporaryDirectory(prefix="cns-imu-gateway-") as directory:
            folder = Path(directory)
            with socket.socket() as unused:
                unused.bind(("127.0.0.1", 0))
                port = unused.getsockname()[1]
            broker_config = folder / "broker.conf"
            broker_config.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
            app_log = (folder / "app.log").open("w+")
            broker_log = (folder / "broker.log").open("w+")
            processes = []
            descriptors = []
            stopped = threading.Event()
            lock = threading.Lock()
            state = {"mode": "noid" if cached_identity else "basic", "tick": 0, "frozen": 1,
                     "last_written_mode": None, "last_sample_tick": 0, "sample_writes": 0}
            errors, messages = [], []
            incoming = queue.Queue()
            streamer = None
            app = None
            try:
                broker = subprocess.Popen(["mosquitto", "-c", str(broker_config)],
                                          stdout=broker_log, stderr=broker_log)
                processes.append(broker)
                self.wait(lambda: self.port_ready(port), "broker start", app_log)
                master, slave = pty.openpty()
                descriptors = [master, slave]
                tty.setraw(slave)
                config = json.loads((ROOT / "config/config.example.json").read_text())
                config["serial"] = {"device": os.ttyname(slave), "baud": 115200}
                config["mqtt"]["connection"].update(host="127.0.0.1", port=port, client_id="imu-test")
                config["mqtt"]["auth"] = {"username": "", "password": ""}
                config["mqtt"]["topics"]["namespace"] = "imutest"
                config["identity"]["binding_file"] = str(folder / "binding.json")
                if cached_identity:
                    (folder / "binding.json").write_text(json.dumps({
                        "schema_version": 1, "device_id": BOX_ID, "device_type": "cns_box"}))
                for channel in config["telemetry_publish"].values():
                    channel["enabled"] = telemetry
                path = folder / "config.json"
                path.write_text(json.dumps(config))
                subscriber = subprocess.Popen(["mosquitto_sub", "-h", "127.0.0.1", "-p", str(port),
                    "-q", "1", "-t", "imutest/#", "-F", "%q %r %t %p"],
                    stdout=subprocess.PIPE, text=True)
                processes.append(subscriber)

                def read_messages():
                    for line in subscriber.stdout:
                        incoming.put(line)
                threading.Thread(target=read_messages, daemon=True).start()

                def stream():
                    while not stopped.wait(0.2):
                        with lock:
                            state["tick"] += 1
                            tick, mode = state["tick"], state["mode"]
                            sample_tick = state["frozen"] if mode == "frozen" else tick
                        try:
                            wire = subprocess.check_output([FIXTURE, str(tick), mode, str(sample_tick)], timeout=2)
                            os.write(master, wire)
                            with lock:
                                state["last_written_mode"] = mode
                                if mode == "samples":
                                    state["last_sample_tick"] = sample_tick
                                    state["sample_writes"] += 1
                            if select.select([master], [], [], 0)[0]:
                                os.read(master, 8192)
                        except (OSError, subprocess.SubprocessError) as error:
                            errors.append(str(error))
                            return
                streamer = threading.Thread(target=stream, daemon=True)
                streamer.start()
                app = subprocess.Popen([BINARY, str(path), "--config-writer=disabled"],
                                       stdout=app_log, stderr=app_log)
                processes.append(app)

                def mode(value):
                    with lock:
                        state["mode"] = value
                        state["frozen"] = state["tick"]

                def drain():
                    while not incoming.empty():
                        qos, retain, topic, payload = incoming.get_nowait().rstrip("\n").split(" ", 3)
                        messages.append({"topic": topic, "qos": int(qos), "retain": int(retain),
                                         "payload": json.loads(payload)})

                def find(topic, predicate=lambda payload: True, after=0):
                    drain()
                    return next((m for m in messages[after:] if m["topic"] == topic
                                 and predicate(m["payload"])), None)

                def wait(topic, predicate=lambda payload: True, after=0):
                    self.wait(lambda: find(topic, predicate, after), f"missing {topic}", app_log, app)
                    return find(topic, predicate, after)["payload"]

                wait(f"{PREFIX}/registration", lambda p: p.get("status") == "online")
                if cached_identity:
                    time.sleep(0.6)
                    self.assertIsNone(find(SAMPLE), "cached SN authorized an unverified source")
                    mode("basic")
                if telemetry:
                    before = wait(REALTIME)
                    self.assertNotIn("imu", before["telemetry"])
                    mode("status")
                    status_only = wait(SNAPSHOT, lambda p: "imu" in p.get("telemetry", {}))
                    self.assertIsNone(status_only["telemetry"]["imu"]["sample"])
                    self.assertTrue(status_only["telemetry"]["imu"]["status"]["diagnostics_available"])
                mode("samples")
                first = wait(SAMPLE)
                self.assertTrue(first["imu"]["sample"]["valid"])
                def sample_pairs():
                    drain()
                    return [m["payload"] for m in messages if m["topic"] == SAMPLE]
                self.wait(lambda: len(sample_pairs()) >= 4,
                          "fewer than four complete sample pairs received", app_log, app)
                pairs = sample_pairs()
                keys = [(p["imu"]["source_epoch"], p["imu"]["sample"]["sample_sequence"]) for p in pairs]
                self.assertEqual(len(keys), len(set(keys)), "UART retransmission published twice")
                for pair in pairs:
                    imu = pair["imu"]
                    self.assertEqual(set(imu), set(contract["pair"]["imu"]))
                    self.assertEqual(set(imu["sample"]), set(contract["pair"]["imu"]["sample"]))
                    self.assertEqual(imu["source"], {"system_id": 218, "component_id": 193})
                    self.assertEqual(imu["sample"]["value"], contract["pair"]["imu"]["sample"]["value"])
                    self.assertLessEqual(imu["sample"]["age_ms"], 200)
                    self.assertEqual(imu["sample"]["valid_for_ms"], 200 - imu["sample"]["age_ms"])
                    for name, kind in [("raw_payload_hex", 0), ("parsed_payload_hex", 1)]:
                        text = imu[name]
                        self.assertRegex(text, r"^[0-9a-f]{128}$")
                        wire = bytes.fromhex(text)
                        self.assertEqual(wire[0:2], bytes([1, kind]))
                        self.assertEqual(int.from_bytes(wire[4:8], "little"), imu["sample"]["sample_sequence"])
                if telemetry:
                    realtime = wait(REALTIME, lambda p: p.get("telemetry", {}).get("imu", {}).get("sample") is not None)
                    snapshot = wait(SNAPSHOT, lambda p: p.get("telemetry", {}).get("imu", {}).get("sample") is not None)
                    self.assertEqual(realtime["schema_version"], 1)
                    self.assertEqual(snapshot["schema_version"], 3)
                    self.assertNotIn("status", realtime["telemetry"]["imu"])
                    self.assertNotIn("raw_payload_hex", snapshot["telemetry"]["imu"])
                    self.assertIn("attitude", realtime["telemetry"])
                    runtime = wait(f"{PREFIX}/runtime/status/v1")
                    self.assertEqual(len(runtime), 11)
                    mode("frozen")
                    drain()
                    after = len(messages)
                    stale = wait(REALTIME, lambda p: p.get("telemetry", {}).get("imu", {}).get("sample", {})
                                 .get("invalid_reason") == "sample_stale", after)
                    self.assertIsNone(stale["telemetry"]["imu"]["sample"]["value"])
                    mode("samples")
                    wait(SAMPLE, after=len(messages))
                    mode("withdraw")
                    after = len(messages)
                    revoked = wait(REALTIME, lambda p: p.get("telemetry", {}).get("imu", {}).get("sample", {})
                                   .get("invalid_reason") == "source_unavailable", after)
                    self.assertIsNone(revoked["telemetry"]["imu"]["sample"]["value"])
                    mode("basic")
                    time.sleep(0.3)
                    drain()
                    after = len(messages)
                    broker.terminate()
                    broker.wait(timeout=3)
                    time.sleep(0.2)
                    with lock:
                        writes_before = state["sample_writes"]
                    mode("samples")
                    def offline_pairs_sent():
                        with lock:
                            return state["sample_writes"] >= writes_before + 2
                    self.wait(offline_pairs_sent, "no complete samples sent during outage", app_log, app)
                    mode("basic")
                    def sampling_stopped():
                        with lock:
                            return state["last_written_mode"] == "basic"
                    self.wait(sampling_stopped, "fixture did not stop offline sampling", app_log, app)
                    with lock:
                        offline_sequence = state["last_sample_tick"]
                    broker = subprocess.Popen(["mosquitto", "-c", str(broker_config)],
                                              stdout=broker_log, stderr=broker_log)
                    processes.append(broker)
                    wait(f"{PREFIX}/registration", lambda p: p.get("status") == "online", after)
                    resumed = wait(REALTIME, lambda p: p.get("telemetry", {}).get("imu", {})
                                   .get("sample", {}).get("sample_sequence") == offline_sequence, after)
                    self.assertGreater(resumed["telemetry"]["imu"]["sample"]["sample_sequence"],
                                       revoked["telemetry"]["imu"]["sample"]["sample_sequence"],
                                       "outage scenario did not actually deliver any offline sample")
                    self.assertEqual(resumed["telemetry"]["imu"]["source_epoch"], first["imu"]["source_epoch"])
                    self.assertIsNone(find(SAMPLE, after=after), "sample backlog replayed after reconnect")
                    mode("samples")
                    live = wait(SAMPLE, lambda p: p["imu"]["sample"]["sample_sequence"] > offline_sequence, after)
                    self.assertEqual(live["imu"]["source_epoch"], first["imu"]["source_epoch"])
                    mode("foreign")
                    drain()
                    after = len(messages)
                    time.sleep(0.5)
                    self.assertIsNone(find(SAMPLE, after=after), "foreign endpoint was accepted")
                    if os.environ.get("CNS_IMU_EVIDENCE"):
                        Path(os.environ["CNS_IMU_EVIDENCE"]).write_text(json.dumps({
                            "evidence": "synthetic F407, real cns_rpi process and isolated MQTT",
                            "pair": first, "realtime": realtime, "snapshot": snapshot,
                            "expired": stale, "revoked": revoked}, ensure_ascii=False, indent=2))
                else:
                    self.assertIsNone(find(REALTIME))
                    self.assertIsNone(find(SNAPSHOT))
                drain()
                for message in messages:
                    if message["topic"] in {SAMPLE, SNAPSHOT, REALTIME}:
                        self.assertEqual(message["qos"], 0)
                        self.assertEqual(message["retain"], 0)
                        self.assertEqual(message["payload"]["device_id"], BOX_ID)
                mode("basic")
                time.sleep(0.3)
                late = subprocess.run(["mosquitto_sub", "-h", "127.0.0.1", "-p", str(port),
                    "-t", SAMPLE, "-C", "1", "-W", "1"], capture_output=True, text=True, timeout=2)
                self.assertFalse(late.stdout, "sample retained by broker")
                self.assertNotEqual(late.returncode, 0)
                self.assertIsNone(app.poll())
                self.assertFalse(errors, errors)
            finally:
                stopped.set()
                if streamer:
                    streamer.join(timeout=3)
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

    def wait(self, predicate, reason, log, process=None):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            if predicate():
                return
            if process and process.poll() is not None:
                break
            time.sleep(0.02)
        log.flush()
        log.seek(0)
        self.fail(reason + "\n" + log.read()[-5000:])

    @staticmethod
    def port_ready(port):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return True
        except OSError:
            return False


if __name__ == "__main__":
    unittest.main()
