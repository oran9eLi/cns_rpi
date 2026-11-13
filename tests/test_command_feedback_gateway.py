"""Actual gateway on a PTY and local broker; no physical F407 is used."""
import json
from datetime import datetime, timedelta, timezone
import os
from pathlib import Path
import pty
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

BINARY, STREAM_FIXTURE, FEEDBACK_FIXTURE = sys.argv[1:4]
del sys.argv[1:4]
ROOT = Path(__file__).resolve().parents[1]
BOX_ID = 'DCDWCNS1GNSSTEST0001'
PREFIX = 'feedbacktest/' + BOX_ID + '/'


class FeedbackGatewayTest(unittest.TestCase):
    def test_final_command_results_reach_f407(self):
        if any(not shutil.which(name) for name in ('mosquitto', 'mosquitto_pub')):
            self.skipTest('requires Mosquitto and clients')
        with tempfile.TemporaryDirectory(prefix='cns-feedback-') as directory:
            folder = Path(directory)
            with socket.socket() as available:
                available.bind(('127.0.0.1', 0))
                port = available.getsockname()[1]
            broker_config = folder / 'broker.conf'
            broker_config.write_text(f'listener {port} 127.0.0.1\nallow_anonymous true\n')
            master, slave = pty.openpty()
            tty.setraw(slave)
            wire = bytearray()
            lock = threading.Lock()
            stopped = threading.Event()
            verified = threading.Event()
            paused = threading.Event()
            config = json.loads((ROOT / 'config/config.example.json').read_text())
            config['serial'] = {'device': os.ttyname(slave), 'baud': 115200}
            config['rtk_base']['enabled'] = False
            config['gnss']['enabled'] = False
            config['identity']['binding_file'] = str(folder / 'binding.json')
            config['mqtt']['connection'].update(host='127.0.0.1', port=port,
                                                client_id='feedback-gateway-test')
            config['mqtt']['auth'] = {'username': '', 'password': ''}
            config['mqtt']['topics']['namespace'] = 'feedbacktest'
            config['logging']['file'] = ''
            path = folder / 'config.json'
            path.write_text(json.dumps(config))
            processes = []
            with (folder / 'broker.log').open('w+') as broker_log, \
                    (folder / 'app.log').open('w+') as app_log:
                try:
                    broker = subprocess.Popen(['mosquitto', '-c', str(broker_config)],
                                              stdout=broker_log, stderr=broker_log)
                    processes.append(broker)

                    def ready():
                        try:
                            with socket.create_connection(('127.0.0.1', port), timeout=0.1):
                                return True
                        except OSError:
                            return False

                    self.wait(ready, 'Broker did not start', app_log)

                    def stream():
                        tick = 0
                        while not stopped.is_set():
                            tick += 1
                            if not paused.is_set():
                                arguments = ([STREAM_FIXTURE, str(tick)] if verified.is_set()
                                             else [FEEDBACK_FIXTURE, 'unverified'])
                                data = subprocess.check_output(arguments, timeout=2)
                                with lock:
                                    os.write(master, data)
                            deadline = time.monotonic() + 0.2
                            while not stopped.is_set() and time.monotonic() < deadline:
                                if select.select([master], [], [], 0.03)[0]:
                                    data = os.read(master, 65536)
                                    with lock:
                                        wire.extend(data)

                    streamer = threading.Thread(target=stream, daemon=True)
                    streamer.start()
                    app = subprocess.Popen([BINARY, str(path), '--config-writer=direct'],
                                           stdout=app_log, stderr=app_log)
                    processes.append(app)
                    self.wait(lambda: (folder / 'binding.json').exists() and
                              'MQTT已连接' in (folder / 'app.log').read_text(),
                              'Gateway did not connect', app_log)
                    time.sleep(0.3)

                    def send(suffix, request):
                        subprocess.run(['mosquitto_pub', '-h', '127.0.0.1', '-p', str(port),
                            '-q', '2', '-t', PREFIX + suffix, '-s'], input=json.dumps(request),
                            text=True, check=True, timeout=3)

                    def decoded():
                        with lock:
                            data = bytes(wire)
                        return json.loads(subprocess.check_output(
                            [FEEDBACK_FIXTURE, 'decode'], input=data, timeout=2))

                    def feedback():
                        return [item for item in decoded() if item['kind'] == 'feedback']

                    unverified_config = {'command_id': 'before-verification', 'parameters': {
                        'topic_publish_intervals_ms': {'telemetry/snapshot/v1': 400}}}
                    send('config/set', unverified_config)
                    self.wait(lambda: 'command_id=before-verification status=applied' in
                              (folder / 'app.log').read_text(),
                              'Unverified-endpoint configuration did not complete', app_log)
                    time.sleep(0.3)
                    with self.subTest('unverified component must not receive feedback'):
                        self.assertEqual(feedback(), [])
                    with lock:
                        wire.clear()
                    verified.set()
                    self.wait(lambda: '未能保存主控箱串口拓扑关联' in
                              (folder / 'app.log').read_text(),
                              'Exact F407 endpoint did not verify', app_log)
                    send('config/set', unverified_config)
                    time.sleep(0.3)
                    self.assertEqual(feedback(), [], 'Offline result replayed after verification')

                    send('config/set', {'command_id': 'rate-ok', 'parameters': {
                        'topic_publish_intervals_ms': {'telemetry/snapshot/v1': 500}}})
                    self.wait(lambda: len(feedback()) == 1,
                              'Successful configuration produced no F407 feedback', app_log)
                    self.assertEqual(feedback()[0], {'kind': 'feedback', 'version': 1,
                        'result': 1, 'length': 2, 'system': 42, 'component': 191,
                        'target_system': 42, 'target_component': 193})

                    def expect_next(suffix, request, result):
                        count = len(feedback())
                        send(suffix, request)
                        self.wait(lambda: len(feedback()) == count + 1,
                                  f'Missing feedback for {suffix}', app_log)
                        self.assertEqual(feedback()[-1]['result'], result)

                    expect_next('config/set', {'command_id': 'rate-bad', 'parameters': {
                        'topic_publish_intervals_ms': {'telemetry/snapshot/v1': 1}}}, 0)
                    count = len(feedback())
                    send('config/set', {'command_id': 'rate-ok', 'parameters': {
                        'topic_publish_intervals_ms': {'telemetry/snapshot/v1': 500}}})
                    time.sleep(0.4)
                    self.assertEqual(len(feedback()), count, 'Config retry repeated its prompt')

                    session = str(uuid.uuid4())

                    def experiment_request(operation, parameters=None):
                        action_id = str(uuid.uuid4())
                        request = {'schema_version': 1, 'command_id': str(uuid.uuid4()),
                            'request_id': action_id, 'action_id': action_id, 'session_id': session,
                            'target': {'device_id': BOX_ID}, 'lease_version': 1,
                            'operation': operation, 'expires_at':
                            (datetime.now(timezone.utc) + timedelta(seconds=20))
                            .isoformat(timespec='milliseconds').replace('+00:00', 'Z')}
                        if parameters is not None:
                            request['parameters'] = parameters
                        return request

                    request = experiment_request('gnss_status')
                    expect_next('experiment/set', request, 1)
                    count = len(feedback())
                    request['command_id'] = str(uuid.uuid4())
                    send('experiment/set', request)
                    time.sleep(0.4)
                    self.assertEqual(len(feedback()), count, 'Action retry repeated its prompt')
                    expect_next('experiment/set', experiment_request('gnss_capture_start',
                        {'capture_id': str(uuid.uuid4()), 'duration_seconds': 1}), 0)
                    invalid = experiment_request('gnss_capture_start',
                        {'capture_id': str(uuid.uuid4()), 'duration_seconds': 0})
                    expect_next('experiment/set', invalid, 0)
                    count = len(feedback())
                    invalid['command_id'] = str(uuid.uuid4())
                    send('experiment/set', invalid)
                    time.sleep(0.4)
                    with self.subTest('rejected parameters retain their action ID'):
                        self.assertEqual(len(feedback()), count, 'Invalid action retry repeated prompt')
                    expect_next('experiment/set', [], 0)

                    def start_control(command_id):
                        before = len([item for item in decoded() if item['kind'] == 'control'])
                        send('control/set', {'command_id': command_id, 'command': 'set_motor_pwm',
                            'parameters': {'pwm_us': [1000, 1000, 1000, 1000]}})
                        self.wait(lambda: len([item for item in decoded()
                            if item['kind'] == 'control']) == before + 1,
                            'Control command did not reach F407', app_log)

                    def reply(result):
                        data = subprocess.check_output([FEEDBACK_FIXTURE, str(result)], timeout=2)
                        with lock:
                            os.write(master, data)

                    count = len(feedback())
                    start_control('control-success')
                    time.sleep(0.2)
                    self.assertEqual(len(feedback()), count, 'Command receipt prompted early')
                    reply(5)  # MAV_RESULT_IN_PROGRESS
                    time.sleep(0.2)
                    self.assertEqual(len(feedback()), count, 'Progress prompted early')
                    reply(0)  # MAV_RESULT_ACCEPTED
                    self.wait(lambda: len(feedback()) == count + 1,
                              'Final control acceptance did not prompt', app_log)
                    self.assertEqual(feedback()[-1]['result'], 1)
                    time.sleep(0.2)

                    count = len(feedback())
                    start_control('control-denied')
                    reply(2)  # MAV_RESULT_DENIED
                    self.wait(lambda: len(feedback()) == count + 1,
                              'Final control rejection did not prompt', app_log)
                    self.assertEqual(feedback()[-1]['result'], 0)
                    time.sleep(0.2)

                    count = len(feedback())
                    start_control('control-timeout')
                    self.wait(lambda: len(feedback()) == count + 1,
                              'Control timeout did not prompt', app_log)
                    self.assertEqual(feedback()[-1]['result'], 0)
                    expect_next('control/set', {'command_id': 'unsupported',
                                               'command': 'unsupported'}, 0)
                    self.assertTrue(all(item['length'] == 2 and item['version'] == 1
                                        for item in feedback()))

                    paused.set()
                    time.sleep(8.8)
                    count = len(feedback())
                    before = len([item for item in decoded() if item['kind'] == 'control'])
                    send('control/set', {'command_id': 'disconnect-pending', 'command': 'land'})
                    self.wait(lambda: len([item for item in decoded()
                        if item['kind'] == 'control']) == before + 1,
                        'Disconnect test control was not pending', app_log)
                    self.wait(lambda: '受控设备串口断开' in (folder / 'app.log').read_text(),
                              'Silent F407 link did not disconnect', app_log)
                    time.sleep(0.2)
                    with self.subTest('disconnect abort must not write old feedback'):
                        self.assertEqual(len(feedback()), count)
                    self.assertIsNone(app.poll())
                finally:
                    stopped.set()
                    if 'streamer' in locals():
                        streamer.join(timeout=3)
                    for process in reversed(processes):
                        if process.poll() is None:
                            process.terminate()
                            process.wait(timeout=5)
                    os.close(master)
                    os.close(slave)

    def wait(self, predicate, message, log, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.05)
        log.flush()
        log.seek(0)
        self.fail(message + '\n' + log.read())


if __name__ == '__main__':
    unittest.main()
