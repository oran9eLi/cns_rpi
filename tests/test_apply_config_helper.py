#!/usr/bin/env python3
"""生产配置 helper 的纯文件系统行为测试。"""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


HELPER_PATH = Path(__file__).parents[1] / "scripts" / "cns-rpi-apply-config.py"


def load_helper():
    spec = importlib.util.spec_from_file_location("cns_rpi_apply_config", HELPER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("无法加载配置 helper")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ApplyConfigHelperTest(unittest.TestCase):
    def test_valid_candidate_replaces_target(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "config.json"
            candidate = root / ".config.json.tmp.123456"
            target.write_text('{"old": true}\n', encoding="utf-8")
            candidate.write_text('{"new": true}\n', encoding="utf-8")

            helper.apply_candidate(candidate, target, root, target)

            self.assertEqual(json.loads(target.read_text(encoding="utf-8")), {"new": True})
            self.assertTrue(candidate.exists())

    def test_rejects_symbolic_link_candidate(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "config.json"
            payload = root / "payload.json"
            candidate = root / ".config.json.tmp.123456"
            target.write_text('{"old": true}\n', encoding="utf-8")
            payload.write_text('{"new": true}\n', encoding="utf-8")
            candidate.symlink_to(payload)

            with self.assertRaises(OSError):
                helper.apply_candidate(candidate, target, root, target)

            self.assertEqual(json.loads(target.read_text(encoding="utf-8")), {"old": True})

    def test_rejects_candidate_outside_allowed_directory(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            allowed = root / "allowed"
            allowed.mkdir()
            target = allowed / "config.json"
            target.write_text('{"old": true}\n', encoding="utf-8")
            candidate = root / ".config.json.tmp.123456"
            candidate.write_text('{"new": true}\n', encoding="utf-8")

            with self.assertRaises(ValueError):
                helper.apply_candidate(candidate, target, allowed, target)

            self.assertEqual(json.loads(target.read_text(encoding="utf-8")), {"old": True})

    def test_rejects_invalid_json_without_touching_target(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "config.json"
            candidate = root / ".config.json.tmp.123456"
            target.write_text('{"old": true}\n', encoding="utf-8")
            candidate.write_text('{', encoding="utf-8")

            with self.assertRaises((ValueError, json.JSONDecodeError)):
                helper.apply_candidate(candidate, target, root, target)

            self.assertEqual(json.loads(target.read_text(encoding="utf-8")), {"old": True})


if __name__ == "__main__":
    unittest.main()
