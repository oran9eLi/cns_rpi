import importlib.util
import os
import tempfile
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

class BuildShellTests(unittest.TestCase):
    @unittest.skipIf(os.name != 'posix' or os.getuid() == 0, 'ordinary-user execution check')
    def test_build_commands_execute_with_ordinary_identity_and_writable_logs(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            (root/'test-logs').mkdir()
            (root/'bin').mkdir()
            command = '#!/bin/bash\nset -eu\ntest "$(id -u)" -ne 0\nprintf "%s\\n" "$0 $*" >> "$CALL_LOG"\nif [ "${1:-}" = -c ]; then echo 1.0.5; fi\n'
            for name in ('cmake','python3'):
                path=root/'bin'/name; path.write_text(command); path.chmod(0o755)
            result=subprocess.run(['bash', str(ROOT/'scripts/ci/build_release.sh'), 'a'*40],cwd=root,
                env={**os.environ, 'PATH':str(root/'bin')+':'+os.environ['PATH'], 'CALL_LOG':str(root/'calls.log')},capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            calls=(root/'calls.log').read_text()
            self.assertIn('scripts/ci/run_tests.py --build build-ci', calls)
            self.assertIn('--git-commit '+'a'*40, calls)
            self.assertTrue((root/'test-logs/tests.log').exists())

if __name__=='__main__': unittest.main()
