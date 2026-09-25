"""发布入口验证：版本查询和配置预检不得进入硬件运行循环。"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = sys.argv.pop(1)
ROOT = Path(__file__).resolve().parents[1]


class ReleaseCliTest(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([BINARY, *args], capture_output=True, text=True,
                              timeout=5)

    def test_version_without_configuration(self):
        result = self.run_cli("--version")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("1.0.3", result.stdout)

    def test_valid_configuration_without_hardware(self):
        result = self.run_cli("--check-config", str(ROOT / "config/config.example.json"))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_configuration_is_rejected_without_echoing_secrets(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            path.write_text(json.dumps({"password": "test-secret-do-not-print"}))
            result = self.run_cli("--check-config", str(path))
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("test-secret-do-not-print", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
