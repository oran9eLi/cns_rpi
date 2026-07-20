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
class OwnershipRestoreTest(unittest.TestCase):
    """以 root 写入后必须还原属主与权限，否则以 dcdw 运行的主程序读不到 0600 的配置。"""

    def test_restores_permission_bits(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "config.json"
            target.write_text('{"a": 1}\n', encoding="utf-8")
            target.chmod(0o600)
            reference = target.stat()

            # 模拟原子替换后暂存文件带来的宽松权限。
            target.chmod(0o644)
            helper.restore_ownership(target, reference)

            self.assertEqual(target.stat().st_mode & 0o777, 0o600)


class ReadonlyMountDetectionTest(unittest.TestCase):
    """判断错了会去 remount 不该动的挂载点，或漏掉只读导致写入直接失败。"""

    def _mounts(self, directory: Path, content: str) -> Path:
        path = directory / "mounts"
        path.write_text(content, encoding="utf-8")
        return path

    def test_returns_none_when_directory_is_not_a_mount_point(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            plain = Path(directory) / "plain"
            plain.mkdir()
            mounts = self._mounts(
                Path(directory), f"/dev/loop0 {plain} ext4 ro,relatime 0 0\n"
            )
            # 目录不是挂载点时必须原地写入，不能去 remount 别的文件系统。
            self.assertIsNone(helper.readonly_mount_for(plain, mounts))

    def test_returns_none_for_writable_mount(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            mounts = self._mounts(
                Path(directory), "/dev/loop0 / ext4 rw,relatime 0 0\n"
            )
            # "/" 一定是挂载点；已可写就不需要 remount。
            self.assertIsNone(helper.readonly_mount_for(Path("/"), mounts))

    def test_detects_readonly_mount(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            mounts = self._mounts(
                Path(directory), "/dev/loop0 / ext4 ro,relatime 0 0\n"
            )
            self.assertEqual(helper.readonly_mount_for(Path("/"), mounts), Path("/"))

    def test_last_entry_wins_when_mounted_twice(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            mounts = self._mounts(
                Path(directory),
                "/dev/loop0 / ext4 ro,relatime 0 0\n"
                "/dev/loop0 / ext4 rw,relatime 0 0\n",
            )
            # 同一挂载点被覆盖挂载时，生效的是最后一条。
            self.assertIsNone(helper.readonly_mount_for(Path("/"), mounts))

    def test_missing_mounts_file_degrades_to_direct_write(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            self.assertIsNone(
                helper.readonly_mount_for(Path("/"), Path(directory) / "absent")
            )


class StagingSeparationTest(unittest.TestCase):
    """候选在可写暂存目录，替换发生在目标目录——两者不是同一个目录。"""

    def test_replaces_target_from_candidate_in_staging_directory(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            staging = root / "staging"
            config = root / "config"
            staging.mkdir()
            config.mkdir()
            target = config / "config.json"
            candidate = staging / ".config.json.tmp.abc123"
            target.write_text('{"old": true}\n', encoding="utf-8")
            candidate.write_text('{"new": true}\n', encoding="utf-8")

            helper.apply_candidate(candidate, target, staging, target)

            self.assertEqual(json.loads(target.read_text(encoding="utf-8")), {"new": True})
            # 候选留在暂存目录，由调用方负责清理。
            self.assertTrue(candidate.exists())
            # 暂存目录不应残留 apply 中间文件。
            leftovers = [p.name for p in staging.iterdir() if ".apply." in p.name]
            self.assertEqual(leftovers, [])

    def test_rejects_candidate_outside_staging_directory(self):
        helper = load_helper()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            staging = root / "staging"
            config = root / "config"
            staging.mkdir()
            config.mkdir()
            target = config / "config.json"
            target.write_text('{"old": true}\n', encoding="utf-8")
            # 候选放在目标目录而非暂存目录，应被拒绝。
            candidate = config / ".config.json.tmp.abc123"
            candidate.write_text('{"new": true}\n', encoding="utf-8")

            with self.assertRaises(ValueError):
                helper.apply_candidate(candidate, target, staging, target)

            self.assertEqual(json.loads(target.read_text(encoding="utf-8")), {"old": True})


if __name__ == "__main__":
    unittest.main()
