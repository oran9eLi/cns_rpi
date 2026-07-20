#!/usr/bin/env python3
"""校验并原子应用 cns_rpi 运行时配置候选文件。

OverlayFS 只读根文件系统下，根分区的写入会落进内存上层、重启即丢。若直接把
配置写在 overlay 视图里，表现是"配置命令返回成功 ACK 但重启后参数回退"的静默
失败。因此启用 overlay 后，本 helper 把校验通过的内容写进底层持久分区
（overlay 的 lowerdir），而不是 overlay 视图。

刻意不使用 overlayroot-chroot：主程序生成的候选文件位于 overlay 的内存上层，
chroot 进 lowerdir 后那个文件不可见。这里改为"读上层候选 → remount 底层可写
→ 在底层原子替换 → 恢复只读"，与 docs/M7系统化部署设计.md 第 1 节一致。

配置只写 lowerdir、不写 overlay 上层，因此上层不会产生遮蔽 config.json 的副本，
底层的新内容对当前运行的系统立即可见，不必等重启。
"""

import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile


CONFIG_DIR = Path("/home/dcdw/cns_rpi/config")
CONFIG_PATH = CONFIG_DIR / "config.json"
_CANDIDATE_NAME = re.compile(r"^\.config\.json\.tmp\.[A-Za-z0-9]+$")
_PROC_MOUNTS = Path("/proc/mounts")


def detect_overlay_lowerdir(mounts_path: Path = _PROC_MOUNTS) -> Path | None:
    """返回根挂载 overlay 的 lowerdir；未启用 overlay 时返回 None。

    与 overlayroot-chroot 的解析方式保持一致：只认根挂载点上的 overlay，
    并要求解析出的 lowerdir 本身是一个挂载点，避免误把普通目录当成持久层写入。
    """
    try:
        entries = mounts_path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None

    for entry in entries:
        fields = entry.split()
        if len(fields) < 4:
            continue
        _device, mount_point, filesystem, options = fields[0], fields[1], fields[2], fields[3]
        if mount_point != "/" or filesystem != "overlay":
            continue
        for option in options.split(","):
            if not option.startswith("lowerdir="):
                continue
            # lowerdir 可以是冒号分隔的多层，最左边一层是最上面的只读层。
            lowerdir = Path(option[len("lowerdir=") :].split(":")[0])
            if os.path.ismount(lowerdir):
                return lowerdir
            raise ValueError(f"overlay lowerdir 不是挂载点: {lowerdir}")
    return None


def persistent_target(lowerdir: Path, allowed_directory: Path, allowed_target: Path):
    """把 overlay 视图下的配置路径映射到底层持久分区上的同名路径。"""
    directory = lowerdir / allowed_directory.relative_to("/")
    target = lowerdir / allowed_target.relative_to("/")
    if not directory.is_dir():
        raise ValueError(f"底层持久层缺少配置目录: {directory}")
    return directory, target


def _remount(mount_point: Path, writable: bool) -> None:
    mode = "rw" if writable else "ro"
    subprocess.run(
        ["mount", "-o", f"remount,{mode}", str(mount_point)],
        check=True,
        capture_output=True,
    )


def apply_candidate(
    candidate: Path,
    target: Path,
    allowed_directory: Path = CONFIG_DIR,
    allowed_target: Path = CONFIG_PATH,
    persist_directory: Path | None = None,
    persist_target: Path | None = None,
) -> None:
    """校验固定目录内的候选 JSON，并以原子替换方式提交到固定目标。

    persist_directory / persist_target 用于 OverlayFS：候选文件始终在 overlay
    视图里校验和读取，但暂存与原子替换发生在底层持久分区。两者必须位于同一
    文件系统，rename 才具备原子性。非 overlay 环境下留空，行为与原先完全一致。
    """
    allowed_directory = allowed_directory.resolve(strict=True)
    allowed_target = allowed_target.absolute()
    candidate = candidate.absolute()
    target = target.absolute()

    if target != allowed_target:
        raise ValueError("目标配置路径不被允许")
    if candidate.parent.resolve(strict=True) != allowed_directory:
        raise ValueError("候选配置不在允许目录")
    if not _CANDIDATE_NAME.fullmatch(candidate.name):
        raise ValueError("候选配置文件名非法")

    # 校验通过后才切换到持久层，保证上面这几条约束永远针对调用方传入的路径生效，
    # 不会因为 overlay 映射而被绕过。
    write_directory = allowed_directory if persist_directory is None else persist_directory
    write_target = allowed_target if persist_target is None else persist_target
    candidate_fd = os.open(candidate, os.O_RDONLY | os.O_NOFOLLOW)
    staged_path: Path | None = None
    try:
        if not stat.S_ISREG(os.fstat(candidate_fd).st_mode):
            raise ValueError("候选配置不是普通文件")
        with os.fdopen(os.dup(candidate_fd), "rb") as stream:
            content = stream.read()
        parsed = json.loads(content)
        if not isinstance(parsed, dict):
            raise ValueError("配置根节点必须是对象")

        with tempfile.NamedTemporaryFile(
            mode="wb", prefix=".config.json.apply.", dir=write_directory, delete=False
        ) as staged:
            staged_path = Path(staged.name)
            staged.write(content)
            staged.flush()
            os.fsync(staged.fileno())
            staged_stat = os.fstat(staged.fileno())
            os.replace(staged_path, write_target)
            target_stat = os.stat(write_target, follow_symlinks=False)
            if (target_stat.st_dev, target_stat.st_ino) != (
                staged_stat.st_dev,
                staged_stat.st_ino,
            ):
                raise OSError("目标配置未指向已校验的暂存文件")
            staged_path = None
    finally:
        os.close(candidate_fd)
        if staged_path is not None:
            staged_path.unlink(missing_ok=True)
    directory_fd = os.open(write_directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)


def main(arguments: list[str]) -> int:
    if len(arguments) != 2:
        print("用法: cns-rpi-apply-config <候选配置> <目标配置>", file=sys.stderr)
        return 2
    candidate, target = Path(arguments[0]), Path(arguments[1])
    try:
        lowerdir = detect_overlay_lowerdir()
        if lowerdir is None:
            apply_candidate(candidate, target)
            return 0

        persist_directory, persist_target = persistent_target(
            lowerdir, CONFIG_DIR, CONFIG_PATH
        )
        _remount(lowerdir, writable=True)
        try:
            apply_candidate(
                candidate,
                target,
                persist_directory=persist_directory,
                persist_target=persist_target,
            )
            # 数据已 fsync 到文件与目录，这里再 sync 一次覆盖底层设备缓存，
            # 之后才允许恢复只读——现场是直接断电，不能依赖延迟回写。
            os.sync()
        finally:
            # 无论写入成功与否都必须恢复只读，否则设备会停在可写状态，
            # 失去 OverlayFS 的断电保护。
            _remount(lowerdir, writable=False)
    except subprocess.CalledProcessError as error:
        detail = error.stderr.decode("utf-8", "replace").strip() if error.stderr else ""
        print(f"切换持久层读写状态失败: {detail or error}", file=sys.stderr)
        return 1
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"应用配置失败: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
