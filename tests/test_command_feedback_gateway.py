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
        if any(not shutil.which(name) for name in ('mosquitto', 'mosquitto_pub', 'mosquitto_sub')):
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
                    messages = []
                    subscriber = subprocess.Popen(['mosquitto_sub', '-h', '127.0.0.1',
                        '-p', str(port), '-q', '2', '-t', PREFIX + 'control/ack',
                        '-t', PREFIX + 'experiment/ack'],
                        stdout=subprocess.PIPE, text=True)
                    processes.append(subscriber)

                    def receive_acks():
                        for line in subscriber.stdout:
                            messages.append(json.loads(line))

                    threading.Thread(target=receive_acks, daemon=True).start()

                    def acked(command_id, status):
                        return any(item.get('command_id') == command_id and
                                   item.get('status') == status for item in messages)

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

                    def start_control(command_id, command='set_motor_pwm', parameters=None):
                        before = len([item for item in decoded() if item['kind'] == 'control'])
                        send('control/set', {'command_id': command_id, 'command': command,
                            'parameters': parameters if parameters is not None else
                            {'motor_id': 2, 'pwm_us': 1300} if command == 'set_motor_pwm' else {}})
                        self.wait(lambda: len([item for item in decoded()
                            if item['kind'] == 'control']) == before + 1,
                            'Control command did not reach F407', app_log)

                    def reply(result, command=31014, reason=0, target=191):
                        data = subprocess.check_output([FEEDBACK_FIXTURE, str(result),
                            str(command), str(reason), str(target)], timeout=2)
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
                    self.wait(lambda: acked('control-success', 'accepted'),
                              'Control acceptance did not reach MQTT', app_log)
                    time.sleep(0.2)
                    self.assertEqual(len(feedback()), count, 'Accepted control repeated success sound')
                    sent = [item for item in decoded() if item['kind'] == 'control'][-1]
                    self.assertEqual(sent['command'], 31014)
                    self.assertEqual(sent['params'], [2, 1300, 0, 0, 0, 0, 0])
                    self.assertEqual((sent['system'], sent['component'],
                        sent['target_system'], sent['target_component']), (42, 191, 42, 193))

                    count = len(feedback())
                    start_control('control-denied')
                    reply(2, reason=1)  # locked
                    self.wait(lambda: len(feedback()) == count + 1,
                              'Final control rejection did not prompt', app_log)
                    self.assertEqual(feedback()[-1]['result'], 0)
                    self.wait(lambda: acked('control-denied', 'rejected'),
                              'Control rejection did not reach MQTT', app_log)
                    refusal = next(item for item in messages if
                                   item.get('command_id') == 'control-denied' and
                                   item.get('status') == 'rejected')
                    self.assertEqual(refusal['error_code'], 'device_locked')
                    send('control/set', {'command_id': 'control-denied',
                        'command': 'set_motor_pwm', 'parameters': {'motor_id': 2, 'pwm_us': 1300}})
                    time.sleep(0.2)
                    self.assertEqual(len(feedback()), count + 1, 'Refusal replay repeated sound')

                    count = len(feedback())
                    start_control('module-on', 'set_module_enabled',
                                  {'module': 'lora', 'enabled': True})
                    sent = [item for item in decoded() if item['kind'] == 'control'][-1]
                    self.assertEqual(sent, {'kind': 'control', 'command': 31015,
                        'params': [4, 1, 0, 0, 0, 0, 0], 'system': 42, 'component': 191,
                        'target_system': 42, 'target_component': 193})
                    reply(0, 31015)
                    self.wait(lambda: acked('module-on', 'accepted'),
                              'Module acceptance did not reach MQTT', app_log)
                    time.sleep(0.2)
                    self.assertEqual(len(feedback()), count, 'Module acceptance repeated sound')

                    before = decoded()
                    send('control/set', {'command_id': 'module-on', 'command': 'set_module_enabled',
                                         'parameters': {'enabled': True, 'module': 'lora'}})
                    self.wait(lambda: sum(item.get('command_id') == 'module-on' and
                                          item.get('status') == 'accepted' for item in messages) == 2,
                              'Module retry did not replay acceptance', app_log)
                    send('control/set', {'command_id': 'module-on', 'command': 'set_module_enabled',
                                         'parameters': {'module': 'lora', 'enabled': False}})
                    self.wait(lambda: any(item.get('command_id') == 'module-on' and
                                          item.get('error_code') == 'command_id_conflict'
                                          for item in messages),
                              'Reused module ID did not reject the new parameters', app_log)
                    self.assertEqual(decoded(), before, 'Replay or ID conflict wrote to F407')

                    start_control('module-off', 'set_module_enabled',
                                  {'module': 'rid', 'enabled': False})
                    sent = [item for item in decoded() if item['kind'] == 'control'][-1]
                    self.assertEqual(sent['command'], 31015)
                    self.assertEqual(sent['params'], [7, 0, 0, 0, 0, 0, 0])
                    reply(2, 31015, reason=4)
                    self.wait(lambda: acked('module-off', 'rejected') and
                              len(feedback()) == count + 1,
                              'Module rejection did not reach MQTT and F407', app_log)
                    refusal = next(item for item in messages if
                                   item.get('command_id') == 'module-off' and
                                   item.get('status') == 'rejected')
                    self.assertEqual(refusal['error_code'], 'invalid_state')
                    self.assertEqual(feedback()[-1], {'kind': 'feedback', 'version': 1,
                        'result': 0, 'length': 2, 'system': 42, 'component': 191,
                        'target_system': 42, 'target_component': 193})

                    before = decoded()
                    send('control/set', {'command_id': 'module-invalid', 'command': 'set_module_enabled',
                                         'parameters': {'module': 'lora', 'enabled': 'true'}})
                    self.wait(lambda: acked('module-invalid', 'rejected'),
                              'Non-boolean module state did not reject', app_log)
                    time.sleep(0.2)
                    self.assertEqual(decoded(), before, 'Local module rejection wrote to F407')

                    count = len(feedback())
                    start_control('control-timeout')
                    self.wait(lambda: acked('control-timeout', 'timeout'),
                              'Control receipt did not time out', app_log)
                    self.assertEqual(len(feedback()), count, 'Unknown receipt prompted failure')
                    send('control/set', {'command_id': 'unsupported',
                                        'command': 'unsupported', 'parameters': {}})
                    self.wait(lambda: acked('unsupported', 'rejected'),
                              'Unsupported command did not reject', app_log)
                    time.sleep(0.2)
                    self.assertEqual(len(feedback()), count, 'Local refusal prompted F407')

                    start_control('arm', 'arm')
                    self.assertEqual([item for item in decoded() if item['kind'] == 'control'][-1]
                                     ['command'], 31093)
                    reply(0, 31093)
                    self.wait(lambda: acked('arm', 'accepted'), 'Arm was not accepted', app_log)
                    start_control('pending-takeoff', 'takeoff')
                    start_control('priority-disarm', 'disarm')
                    start_control('priority-stop', 'emergency_stop')
                    reply(0, 31090, target=190)
                    reply(0, 31090, target=0)
                    time.sleep(0.15)
                    self.assertFalse(acked('priority-stop', 'accepted'), 'Wrong target ACK matched')
                    reply(0, 31090)
                    reply(0, 31094)
                    reply(0, 31091)
                    for request_id in ('priority-stop', 'priority-disarm', 'pending-takeoff'):
                        self.wait(lambda: acked(request_id, 'accepted'),
                                  'Out of order acceptance missing: ' + request_id, app_log)
                    self.assertEqual(len(feedback()), count, 'Priority acceptance repeated sound')
                    self.assertTrue(all(item['length'] == 2 and item['version'] == 1
                                        for item in feedback()))

                    echo = experiment_request('uart_echo')
                    echo['text'] = 'priority-control-test'
                    echo_count = (folder / 'app.log').read_text().count('H1回显帧已写入串口')
                    send('experiment/set', echo)
                    self.wait(lambda: (folder / 'app.log').read_text().count(
                        'H1回显帧已写入串口') == echo_count + 1,
                        'Echo experiment did not occupy the serial link', app_log)
                    start_control('disarm-preempts-echo', 'disarm')
                    reply(0, 31094)
                    self.wait(lambda: acked('disarm-preempts-echo', 'accepted') and
                              acked(echo['command_id'], 'rejected'),
                              'Priority control did not abort echo and report both receipts', app_log)
                    self.assertEqual(len(feedback()), count,
                                     'Experiment cancellation overrode the disarm prompt')
                    send('experiment/set', echo)
                    time.sleep(0.3)
                    self.assertEqual(len(feedback()), count,
                                     'Cancelled experiment replay produced a failure prompt')

                    paused.set()
                    time.sleep(8.8)
                    count = len(feedback())
                    before = len([item for item in decoded() if item['kind'] == 'control'])
                    send('control/set', {'command_id': 'disconnect-pending', 'command': 'land',
                                         'parameters': {}})
                    self.wait(lambda: len([item for item in decoded()
                        if item['kind'] == 'control']) == before + 1,
                        'Disconnect test control was not pending', app_log)
                    self.wait(lambda: '受控设备串口断开' in (folder / 'app.log').read_text(),
                              'Silent F407 link did not disconnect', app_log)
                    self.wait(lambda: acked('disconnect-pending', 'timeout'),
                              'Lost receipt did not report unknown execution', app_log)
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
