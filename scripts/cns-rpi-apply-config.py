#!/usr/bin/env python3
"""校验并原子应用 cns_rpi 运行时配置候选文件。"""

import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile


CONFIG_DIR = Path("/home/dcdw/cns_rpi/config")
CONFIG_PATH = CONFIG_DIR / "config.json"
_CANDIDATE_NAME = re.compile(r"^\.config\.json\.tmp\.[A-Za-z0-9]+$")


def apply_candidate(
    candidate: Path,
    target: Path,
    allowed_directory: Path = CONFIG_DIR,
    allowed_target: Path = CONFIG_PATH,
) -> None:
    """校验固定目录内的候选 JSON，并以原子替换方式提交到固定目标。"""
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
            mode="wb", prefix=".config.json.apply.", dir=allowed_directory, delete=False
        ) as staged:
            staged_path = Path(staged.name)
            staged.write(content)
            staged.flush()
            os.fsync(staged.fileno())
            staged_stat = os.fstat(staged.fileno())
            os.replace(staged_path, target)
            target_stat = os.stat(target, follow_symlinks=False)
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
    directory_fd = os.open(allowed_directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)


def main(arguments: list[str]) -> int:
    if len(arguments) != 2:
        print("用法: cns-rpi-apply-config <候选配置> <目标配置>", file=sys.stderr)
        return 2
    try:
        apply_candidate(Path(arguments[0]), Path(arguments[1]))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"应用配置失败: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
