import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('driver', ROOT/'scripts/ci/run_tests.py')
driver=importlib.util.module_from_spec(spec); spec.loader.exec_module(driver)

class DriverTests(unittest.TestCase):
    def test_live_child_markers_drive_broker(self):
        class Broker:
            actions=[]
            def stop(self): self.actions.append('stop')
            def start(self): self.actions.append('start')
        broker=Broker()
        result=driver.supervise([sys.executable, '-u', '-c', 'import sys; print("本地Broker首次收发成功；现在停止Broker。",flush=True); print("MQTT断线已检测；现在重启Broker。",flush=True)'], broker, {})
        self.assertEqual(result, 0)
        self.assertEqual(broker.actions, ['stop','start'])
    def test_child_failure_propagates(self):
        self.assertEqual(driver.supervise([sys.executable,'-c','raise SystemExit(7)'], None, {}),7)

if __name__=='__main__': unittest.main()
