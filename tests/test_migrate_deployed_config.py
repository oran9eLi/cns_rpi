#!/usr/bin/env python3
"""部署阶段通过配置 helper 迁移只读配置卷的集成测试。"""

import json
from pathlib import Path
import stat
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]
WRAPPER = ROOT / "scripts" / "migrate_deployed_telemetry_config.sh"
MIGRATOR = ROOT / "scripts" / "migrate_telemetry_config.py"


class MigrateDeployedConfigTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.config_dir = self.root / "config"
        self.staging_dir = self.root / "staging"
        self.config_dir.mkdir()
        self.staging_dir.mkdir()
        self.config_path = self.config_dir / "config.json"
        self.marker = self.root / "helper-called"
        self.helper = self.root / "apply-helper"
        self.helper.write_text(
            textwrap.dedent(
                f"""\
                #!/usr/bin/env python3
                import os
                from pathlib import Path
                import shutil
                import sys

                candidate = Path(sys.argv[1])
                target = Path(sys.argv[2])
                if not candidate.name.startswith(".config.json.tmp."):
                    raise SystemExit(2)
                target.parent.chmod(0o755)
                shutil.copyfile(candidate, target)
                target.parent.chmod(0o555)
                Path({str(self.marker)!r}).write_text("called", encoding="utf-8")
                """
            ),
            encoding="utf-8",
        )
        self.helper.chmod(0o755)

    def tearDown(self):
        self.config_dir.chmod(0o755)
        self.temporary.cleanup()

    def run_wrapper(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "bash",
                str(WRAPPER),
                str(MIGRATOR),
                str(self.helper),
                str(self.config_path),
                str(self.staging_dir),
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_old_configuration_is_applied_through_helper_on_readonly_directory(self):
        old = {
            "runtime": {
                "telemetry_publish_interval_ms": 1000,
                "heartbeat_interval_ms": 1000,
                "applied_command_ids": [],
            },
            "px4_realtime": {"enabled": True, "publish_interval_ms": 50},
            "mqtt": {"topics": {"telemetry": {"suffix": "telemetry", "qos": 0}}},
        }
        self.config_path.write_text(json.dumps(old), encoding="utf-8")
        self.config_dir.chmod(0o555)

        result = self.run_wrapper()

        self.assertEqual(result.returncode, 0, result.stderr)
        migrated = json.loads(self.config_path.read_text(encoding="utf-8"))
        self.assertEqual(migrated["telemetry_publish"]["realtime"]["interval_ms"], 100)
        self.assertTrue(self.marker.exists())
        self.assertEqual(stat.S_IMODE(self.config_dir.stat().st_mode), 0o555)
        self.assertEqual(list(self.staging_dir.iterdir()), [])

    def test_current_configuration_does_not_call_helper_or_rewrite_target(self):
        current = {
            "runtime": {"heartbeat_interval_ms": 1000, "applied_command_ids": []},
            "telemetry_publish": {
                "snapshot": {"enabled": True, "interval_ms": 1000},
                "realtime": {"enabled": True, "interval_ms": 100},
            },
            "mqtt": {
                "topics": {
                    "telemetry_snapshot": {"suffix": "custom/snapshot", "qos": 0},
                    "telemetry_realtime": {"suffix": "custom/realtime", "qos": 0},
                }
            },
        }
        self.config_path.write_text(json.dumps(current), encoding="utf-8")
        before = self.config_path.read_bytes()
        self.config_dir.chmod(0o555)

        result = self.run_wrapper()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.config_path.read_bytes(), before)
        self.assertFalse(self.marker.exists())
        self.assertEqual(list(self.staging_dir.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
