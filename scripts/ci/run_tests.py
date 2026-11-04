#!/usr/bin/env python3
"""Run CTest against a private loopback broker, including reconnect coverage."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

class Broker:
    def __init__(self, directory):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0)); self.port = sock.getsockname()[1]
        self.config = Path(directory)/'mosquitto.conf'
        self.config.write_text(f'listener {self.port} 127.0.0.1\nallow_anonymous true\npersistence false\n')
        self.log = open(Path(directory)/'mosquitto.log', 'ab')
        self.process = None
    def start(self):
        self.process = subprocess.Popen(['mosquitto','-c',str(self.config)], stdout=self.log, stderr=self.log)
        deadline = time.monotonic()+10
        while time.monotonic() < deadline:
            if self.process.poll() is not None: raise RuntimeError('private Mosquitto exited')
            try:
                with socket.create_connection(('127.0.0.1',self.port), timeout=.2): return
            except OSError: time.sleep(.05)
        raise RuntimeError('private Mosquitto did not listen')
    def stop(self):
        if self.process is not None:
            self.process.terminate(); self.process.wait(timeout=10); self.process=None
    def close(self):
        self.stop(); self.log.close()

def supervise(command, broker, environment):
    with subprocess.Popen(command, env={**os.environ, **environment}, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, encoding='utf-8', bufsize=1) as process:
        for line in process.stdout:
            print(line, end='', flush=True)
            if '本地Broker首次收发成功；现在停止Broker。' in line: broker.stop()
            if 'MQTT断线已检测；现在重启Broker。' in line: broker.start()
        return process.wait()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default='build')
    parser.add_argument('--logs', default='test-logs')
    args=parser.parse_args()
    Path(args.logs).mkdir(parents=True, exist_ok=True)
    broker=Broker(args.logs)
    try:
        broker.start()
        environment={'CNS_TEST_MQTT_BROKER_PORT':str(broker.port), 'CNS_TEST_MQTT_RECONNECT_PORT':str(broker.port)}
        result=supervise(['ctest','--test-dir',args.build,'--verbose','--timeout','120'],broker,environment)
        if result: return result
        return subprocess.call([sys.executable,'scripts/test_cellular_dialup.py'])
    finally: broker.close()

if __name__=='__main__': sys.exit(main())
