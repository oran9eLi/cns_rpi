#!/usr/bin/env python3
"""在临时目录验证发布部署流程，所有系统管理命令均由安全替身处理。"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else ROOT / "build" / "cns_rpi"


class ReleaseDeployTest(unittest.TestCase):
    def setUp(self):
        self.assertTrue(BINARY.is_file(), "请先构建 build/cns_rpi")
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repository"
        self.config = self.root / "persistent" / "config.json"
        self.events = self.root / "events"
        for name in ("scripts", "config", "build", "systemd"):
            (self.repo / name).mkdir(parents=True)
        (self.repo / "build" / "cns_rpi").symlink_to(BINARY)
        for name in (
            "migrate_telemetry_config.py",
            "migrate_deployed_telemetry_config.sh",
            "cns-rpi-apply-config.py",
            "cellular_dialup.py",
            "cellular_link.py",
        ):
            shutil.copy2(ROOT / "scripts" / name, self.repo / "scripts" / name)
        for source in (ROOT / "systemd").iterdir():
            if source.is_file():
                shutil.copy2(source, self.repo / "systemd" / source.name)
        script = (ROOT / "scripts" / "deploy.sh").read_text(encoding="utf-8")
        for original, replacement in (
            ("/home/dcdw/cns_rpi", self.repo),
            ("/var/lib/cns-rpi", self.config.parent),
            ("/run/cns-rpi", self.root / "runtime"),
            ("/usr/local", self.root / "local"),
            ("/etc", self.root / "etc"),
        ):
            script = script.replace(original, str(replacement))
        self.deploy = self.repo / "scripts" / "deploy.sh"
        self.deploy.write_text(script, encoding="utf-8")
        self.legacy = self.repo / "config" / "config.json"
        self.example = json.loads((ROOT / "config" / "config.example.json").read_text())
        self.legacy.write_text(json.dumps(self.example), encoding="utf-8")
        stub_dir = self.root / "bin"
        stub_dir.mkdir()
        # 替身只允许安装到临时目录，拒绝未明确模拟的特权命令。
        stub = stub_dir / "stub"
        stub.write_text(
            """#!/usr/bin/python3
import json
import os
from pathlib import Path
import subprocess
import sys

name = Path(sys.argv[0]).name
args = sys.argv[1:]
root = Path(os.environ['DEPLOY_TEST_ROOT'])
if name == 'id':
    print('1000' if args == ['-u'] else 'dcdw')
elif name == 'findmnt':
    print(os.environ.get('DEPLOY_TEST_FSTYPE', 'ext4'))
elif name == 'mountpoint':
    sys.exit(0 if os.environ.get('DEPLOY_TEST_MOUNTED') else 1)
elif name == 'cmake':
    pass
elif name == 'sudo':
    if args in (['-n', 'true'], ['-v']):
        sys.exit(0)
    if args[0] == 'install':
        clean = []
        index = 1
        while index < len(args):
            if args[index] in ('-o', '-g'):
                index += 2
            else:
                clean.append(args[index])
                index += 1
        destination = Path(clean[-1]).resolve()
        if root not in destination.parents:
            raise SystemExit('拒绝向临时目录之外安装')
        sys.exit(subprocess.run(['/usr/bin/install', *clean]).returncode)
    if args[0] == 'cmp':
        sys.exit(subprocess.run(['/usr/bin/cmp', *args[1:]]).returncode)
    if args[0] == 'systemctl':
        sys.exit(subprocess.run(args).returncode)
    raise SystemExit('未允许的 sudo 命令：' + repr(args))
elif name == 'systemctl':
    with (root / 'events').open('a') as output:
        output.write(json.dumps(args) + '\\n')
    if args[0] == 'is-active':
        sys.exit(0 if (root / ('active-' + args[-1])).exists() else 3)
    if args[0] in ('start', 'restart') and args[-1] in (
        'cns-rpi.service', 'cellular-dialup.service'
    ):
        config = root / 'persistent' / 'config.json'
        result = subprocess.run([os.environ['DEPLOY_TEST_BINARY'], '--check-config', str(config)])
        if result.returncode:
            raise SystemExit('启动服务前现场配置无效')
        (root / ('active-' + args[-1])).touch()
else:
    raise SystemExit('未允许的系统命令')
""",
            encoding="utf-8",
        )
        stub.chmod(0o755)
        for name in ("id", "findmnt", "mountpoint", "cmake", "sudo", "systemctl"):
            (stub_dir / name).symlink_to(stub)
        self.env = dict(os.environ)
        self.env.update(
            PATH=str(stub_dir) + os.pathsep + os.environ["PATH"],
            DEPLOY_TEST_ROOT=str(self.root),
            DEPLOY_TEST_BINARY=str(BINARY),
        )

    def run_deploy(self):
        return subprocess.run(
            ["bash", str(self.deploy)], env=self.env, text=True,
            capture_output=True, check=False, timeout=30,
        )

    def commands(self):
        return [json.loads(line) for line in self.events.read_text().splitlines()] if self.events.exists() else []

    def test_first_deploy_migrates_configuration_before_starting_both_services(self):
        old = self.example
        old.pop("telemetry_publish")
        old["runtime"]["telemetry_publish_interval_ms"] = 1700
        old["px4_realtime"] = {"enabled": True, "publish_interval_ms": 50}
        topics = old["mqtt"]["topics"]
        topics.pop("telemetry_snapshot")
        topics.pop("telemetry_realtime")
        topics["telemetry"] = {"suffix": "telemetry", "qos": 0}
        self.legacy.write_text(json.dumps(old), encoding="utf-8")

        result = self.run_deploy()

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        current = json.loads(self.config.read_text())
        self.assertEqual(current["telemetry_publish"]["snapshot"]["interval_ms"], 1700)
        self.assertEqual(current["telemetry_publish"]["realtime"]["interval_ms"], 100)
        self.assertFalse(self.legacy.exists())
        self.assertTrue(self.legacy.with_suffix(".json.migrated").exists())
        for service in ("cns-rpi.service", "cellular-dialup.service"):
            self.assertIn(["enable", service], self.commands())
            self.assertIn(["start", service], self.commands())
            self.assertTrue((self.root / "etc" / "systemd" / "system" / service).exists())

    def test_repeated_deploy_preserves_site_configuration_and_restarts_services(self):
        first = self.run_deploy()
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        current = json.loads(self.config.read_text())
        current["mqtt"]["connection"]["host"] = "site.example.com"
        current["telemetry_publish"]["snapshot"]["interval_ms"] = 2300
        self.config.write_text(json.dumps(current), encoding="utf-8")
        before = self.config.read_bytes()
        self.events.unlink()

        result = self.run_deploy()

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.config.read_bytes(), before)
        for service in ("cns-rpi.service", "cellular-dialup.service"):
            self.assertIn(["enable", service], self.commands())
            self.assertIn(["restart", service], self.commands())

    def test_invalid_initial_config_does_not_write_persistent_config_or_start_services(self):
        self.example["mqtt"]["connection"]["port"] = -1
        self.legacy.write_text(json.dumps(self.example), encoding="utf-8")
        result = self.run_deploy()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.config.exists())
        self.assertTrue(self.legacy.exists())
        self.assertEqual(self.commands(), [])

    def test_invalid_existing_config_is_preserved_without_starting_services(self):
        self.config.parent.mkdir()
        self.config.write_text('{"invalid":true}', encoding="utf-8")
        before = self.config.read_bytes()
        result = self.run_deploy()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.config.read_bytes(), before)
        self.assertEqual(self.commands(), [])

    def test_missing_apn_does_not_write_persistent_config_or_start_services(self):
        self.example["cellular"].pop("apn")
        self.legacy.write_text(json.dumps(self.example), encoding="utf-8")
        result = self.run_deploy()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.config.exists())
        self.assertTrue(self.legacy.exists())
        self.assertEqual(self.commands(), [])

    def test_overlay_root_is_rejected(self):
        self.env["DEPLOY_TEST_FSTYPE"] = "overlay"
        result = self.run_deploy()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OverlayFS", result.stderr)
        self.assertFalse(self.config.exists())
        self.assertEqual(self.commands(), [])

    def test_legacy_mounted_config_volume_is_rejected(self):
        self.env["DEPLOY_TEST_MOUNTED"] = "1"
        result = self.run_deploy()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("旧配置卷", result.stderr)
        self.assertFalse(self.config.exists())
        self.assertEqual(self.commands(), [])

    def test_legacy_config_service_is_rejected_even_when_not_mounted(self):
        legacy_service = self.root / "etc" / "systemd" / "system" / "cns-rpi-config.service"
        legacy_service.parent.mkdir(parents=True)
        legacy_service.touch()
        result = self.run_deploy()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("旧配置卷", result.stderr)
        self.assertFalse(self.config.exists())
        self.assertEqual(self.commands(), [])


if __name__ == "__main__":
    unittest.main()
