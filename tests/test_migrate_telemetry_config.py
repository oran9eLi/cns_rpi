#!/usr/bin/env python3
"""遥测发布配置迁移脚本的行为测试。"""

import json
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "migrate_telemetry_config.py"


class MigrateTelemetryConfigTest(unittest.TestCase):
    def run_migration(self, path: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["python3", str(SCRIPT), str(path)],
            check=False,
            capture_output=True,
            text=True,
        )

    def write_config(self, root: dict) -> tuple[tempfile.TemporaryDirectory, Path]:
        temporary = tempfile.TemporaryDirectory()
        path = Path(temporary.name) / "config.json"
        path.write_text(json.dumps(root, ensure_ascii=False), encoding="utf-8")
        return temporary, path

    def test_migrates_old_telemetry_configuration(self):
        old = {
            "runtime": {
                "telemetry_publish_interval_ms": 2000,
                "heartbeat_interval_ms": 1000,
                "applied_command_ids": [],
            },
            "px4_realtime": {"enabled": True, "publish_interval_ms": 50},
            "mqtt": {
                "topics": {
                    "telemetry": {"suffix": "telemetry", "qos": 0},
                    "registration": {"suffix": "registration", "qos": 2},
                }
            },
        }
        temporary, path = self.write_config(old)
        self.addCleanup(temporary.cleanup)

        result = self.run_migration(path)

        self.assertEqual(result.returncode, 0, result.stderr)
        migrated = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(
            migrated["telemetry_publish"],
            {
                "snapshot": {"enabled": True, "interval_ms": 2000},
                "realtime": {"enabled": True, "interval_ms": 100},
            },
        )
        self.assertNotIn("telemetry_publish_interval_ms", migrated["runtime"])
        self.assertNotIn("px4_realtime", migrated)
        self.assertNotIn("telemetry", migrated["mqtt"]["topics"])
        self.assertEqual(
            migrated["mqtt"]["topics"]["telemetry_snapshot"],
            {"suffix": "telemetry/snapshot/v1", "qos": 0},
        )
        self.assertEqual(
            migrated["mqtt"]["topics"]["telemetry_realtime"],
            {"suffix": "telemetry/realtime/v1", "qos": 0},
        )

    def test_preserves_non_default_old_realtime_interval(self):
        old = {
            "runtime": {
                "telemetry_publish_interval_ms": 1500,
                "heartbeat_interval_ms": 1000,
                "applied_command_ids": [],
            },
            "px4_realtime": {"enabled": False, "publish_interval_ms": 200},
            "mqtt": {"topics": {"telemetry": {"suffix": "telemetry", "qos": 0}}},
        }
        temporary, path = self.write_config(old)
        self.addCleanup(temporary.cleanup)

        result = self.run_migration(path)

        self.assertEqual(result.returncode, 0, result.stderr)
        realtime = json.loads(path.read_text(encoding="utf-8"))["telemetry_publish"]["realtime"]
        self.assertEqual(realtime, {"enabled": False, "interval_ms": 200})

    def test_new_configuration_is_not_rewritten(self):
        new = {
            "runtime": {"heartbeat_interval_ms": 1000, "applied_command_ids": []},
            "telemetry_publish": {
                "snapshot": {"enabled": True, "interval_ms": 1000},
                "realtime": {"enabled": True, "interval_ms": 100},
            },
            "mqtt": {
                "topics": {
                    "telemetry_snapshot": {
                        "suffix": "telemetry/snapshot/v1",
                        "qos": 0,
                    },
                    "telemetry_realtime": {
                        "suffix": "telemetry/realtime/v1",
                        "qos": 0,
                    },
                }
            },
        }
        temporary, path = self.write_config(new)
        self.addCleanup(temporary.cleanup)
        before = path.read_bytes()

        result = self.run_migration(path)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(path.read_bytes(), before)

    def test_new_configuration_with_custom_suffix_is_not_rewritten(self):
        new = {
            "runtime": {"heartbeat_interval_ms": 1000, "applied_command_ids": []},
            "telemetry_publish": {
                "snapshot": {"enabled": True, "interval_ms": 2000},
                "realtime": {"enabled": False, "interval_ms": 200},
            },
            "mqtt": {
                "topics": {
                    "telemetry_snapshot": {"suffix": "custom/snapshot", "qos": 0},
                    "telemetry_realtime": {"suffix": "custom/realtime", "qos": 0},
                }
            },
        }
        temporary, path = self.write_config(new)
        self.addCleanup(temporary.cleanup)
        before = path.read_bytes()

        result = self.run_migration(path)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(path.read_bytes(), before)

    def test_new_configuration_without_runtime_is_left_unchanged_and_rejected(self):
        invalid = {
            "telemetry_publish": {
                "snapshot": {"enabled": True, "interval_ms": 1000},
                "realtime": {"enabled": True, "interval_ms": 100},
            },
            "mqtt": {
                "topics": {
                    "telemetry_snapshot": {
                        "suffix": "telemetry/snapshot/v1",
                        "qos": 0,
                    },
                    "telemetry_realtime": {
                        "suffix": "telemetry/realtime/v1",
                        "qos": 0,
                    },
                }
            },
        }
        temporary, path = self.write_config(invalid)
        self.addCleanup(temporary.cleanup)
        before = path.read_bytes()

        result = self.run_migration(path)

        self.assertEqual(result.returncode, 1)
        self.assertEqual(path.read_bytes(), before)

    def test_invalid_old_configuration_is_left_unchanged(self):
        invalid = {
            "px4_realtime": {"enabled": True, "publish_interval_ms": 50},
            "mqtt": {"topics": {"telemetry": {"suffix": "telemetry", "qos": 0}}},
        }
        temporary, path = self.write_config(invalid)
        self.addCleanup(temporary.cleanup)
        before = path.read_bytes()

        result = self.run_migration(path)

        self.assertEqual(result.returncode, 1)
        self.assertEqual(path.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
