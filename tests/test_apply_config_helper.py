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


class OverlayLowerdirDetectionTest(unittest.TestCase):
    """OverlayFS 持久层解析：解析错了会把配置写进内存层，重启后静默丢失。"""

    def _mounts(self, directory: Path, content: str) -> Path:
        path = directory / "mounts"
        path.write_text(content, encoding="utf-8")
        return path

    def test_returns_none_when_root_is_not_overlay(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            mounts = self._mounts(
                Path(directory),
                "/dev/mmcblk0p2 / ext4 rw,noatime 0 0\n"
                "proc /proc proc rw,nosuid 0 0\n",
            )
            self.assertIsNone(helper.detect_overlay_lowerdir(mounts))

    def test_ignores_overlay_that_is_not_mounted_on_root(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            mounts = self._mounts(
                Path(directory),
                "/dev/mmcblk0p2 / ext4 rw,noatime 0 0\n"
                "overlay /var/lib/docker/x overlay rw,lowerdir=/tmp 0 0\n",
            )
            self.assertIsNone(helper.detect_overlay_lowerdir(mounts))

    def test_missing_mounts_file_degrades_to_no_overlay(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            self.assertIsNone(
                helper.detect_overlay_lowerdir(Path(directory) / "absent")
            )

    def test_rejects_lowerdir_that_is_not_a_mount_point(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake_lower = root / "not-a-mount"
            fake_lower.mkdir()
            mounts = self._mounts(
                root,
                f"overlayroot / overlay rw,lowerdir={fake_lower},upperdir=/x 0 0\n",
            )
            # 宁可报错也不能把普通目录当持久层：那会让配置写进内存层而不自知。
            with self.assertRaises(ValueError):
                helper.detect_overlay_lowerdir(mounts)

    def test_parses_lowerdir_when_root_is_overlay(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            mounts = self._mounts(
                Path(directory),
                "overlayroot / overlay rw,lowerdir=/,upperdir=/y,workdir=/z 0 0\n",
            )
            # "/" 一定是挂载点，用它验证解析与挂载点校验都走通。
            self.assertEqual(helper.detect_overlay_lowerdir(mounts), Path("/"))


class PersistentTargetMappingTest(unittest.TestCase):
    def test_maps_config_paths_into_lowerdir(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            lowerdir = Path(directory)
            config_dir = Path("/home/dcdw/cns_rpi/config")
            (lowerdir / config_dir.relative_to("/")).mkdir(parents=True)

            persist_dir, persist_target = helper.persistent_target(
                lowerdir, config_dir, config_dir / "config.json"
            )

            self.assertEqual(persist_dir, lowerdir / "home/dcdw/cns_rpi/config")
            self.assertEqual(
                persist_target, lowerdir / "home/dcdw/cns_rpi/config/config.json"
            )

    def test_rejects_lowerdir_without_config_directory(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            config_dir = Path("/home/dcdw/cns_rpi/config")
            with self.assertRaises(ValueError):
                helper.persistent_target(
                    Path(directory), config_dir, config_dir / "config.json"
                )


class PersistentWriteTest(unittest.TestCase):
    """校验始终针对 overlay 视图，写入落到持久层。"""

    def test_writes_to_persist_target_not_overlay_view(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            view = root / "view"
            persist = root / "persist"
            view.mkdir()
            persist.mkdir()

            view_target = view / "config.json"
            persist_target = persist / "config.json"
            candidate = view / ".config.json.tmp.abc123"
            view_target.write_text('{"view": "stale"}\n', encoding="utf-8")
            persist_target.write_text('{"persist": "old"}\n', encoding="utf-8")
            candidate.write_text('{"new": true}\n', encoding="utf-8")

            helper.apply_candidate(
                candidate,
                view_target,
                view,
                view_target,
                persist_directory=persist,
                persist_target=persist_target,
            )

            # 新内容进持久层；overlay 视图里的旧文件不被触碰。
            self.assertEqual(
                json.loads(persist_target.read_text(encoding="utf-8")), {"new": True}
            )
            self.assertEqual(
                json.loads(view_target.read_text(encoding="utf-8")), {"view": "stale"}
            )

    def test_path_validation_still_applies_when_persisting(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            view = root / "view"
            persist = root / "persist"
            view.mkdir()
            persist.mkdir()
            view_target = view / "config.json"
            view_target.write_text('{"old": true}\n', encoding="utf-8")
            (persist / "config.json").write_text('{"old": true}\n', encoding="utf-8")
            # 候选文件名非法，持久化路径不应成为绕过校验的旁路。
            candidate = view / "config.json.evil"
            candidate.write_text('{"new": true}\n', encoding="utf-8")

            with self.assertRaises(ValueError):
                helper.apply_candidate(
                    candidate,
                    view_target,
                    view,
                    view_target,
                    persist_directory=persist,
                    persist_target=persist / "config.json",
                )

            self.assertEqual(
                json.loads((persist / "config.json").read_text(encoding="utf-8")),
                {"old": True},
            )


if __name__ == "__main__":
    unittest.main()
