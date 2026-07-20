#!/usr/bin/env python3
"""校验并原子应用 cns_rpi 运行时配置候选文件。

只读根文件系统方案下，配置目录是一个独立文件系统的挂载点，平时以 ro 挂载，
只在写配置时短暂切为 rw。配置刻意不放在 OverlayFS 覆盖的根分区上：overlayfs
会把自己的 lowerdir 钉住，写完之后无法再把它 remount 回只读（EBUSY），文件
系统会停在可写状态、断电保护失效——上游 overlayroot-chroot 有同样限制。独立
文件系统不参与 overlay，读写状态完全自主，remount 回 ro 才能真正成功。

详见 docs/OverlayFS只读根文件系统设计.md。

配置目录尚未成为独立挂载点时（开发机、迁移过渡期），本 helper 直接原地写入，
行为与改造前完全一致。
"""

import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile


# 运行时配置不放在 git 工作树里：它是可变的现场状态，FHS 里属于 /var/lib。
# 该目录同时是独立 ext4 文件系统的挂载点，平时只读，写配置时短暂切可写——
# 这是 OverlayFS 下唯一能真正恢复只读的做法，详见
# docs/OverlayFS只读根文件系统设计.md。
CONFIG_DIR = Path("/var/lib/cns-rpi")
CONFIG_PATH = CONFIG_DIR / "config.json"
# 候选文件的暂存目录：配置目录平时只读，候选不能建在那里。
# 由 systemd 的 RuntimeDirectory=cns-rpi 创建于 tmpfs，重启自动清空。
STAGING_DIR = Path("/run/cns-rpi")
_CANDIDATE_NAME = re.compile(r"^\.config\.json\.tmp\.[A-Za-z0-9]+$")
_PROC_MOUNTS = Path("/proc/mounts")


def readonly_mount_for(directory: Path, mounts_path: Path = _PROC_MOUNTS) -> Path | None:
    """目录本身是只读挂载点时返回它，否则返回 None。

    只认"目录恰好是挂载点"这一种情况：配置目录是独立文件系统的挂载点，
    读写状态与根文件系统无关，因此可以在写入前后精确切换。
    目录不是挂载点（开发机、迁移前的现场）时返回 None，直接原地写入，
    行为与改造前完全一致。
    """
    if not os.path.ismount(directory):
        return None
    try:
        entries = mounts_path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None

    resolved = str(directory)
    read_only: bool | None = None
    for entry in entries:
        fields = entry.split()
        if len(fields) < 4 or fields[1] != resolved:
            continue
        # 同一挂载点可能被挂载多次，最后一条生效，因此遍历完再判断。
        read_only = "ro" in fields[3].split(",")
    return directory if read_only else None


def _remount(mount_point: Path, writable: bool) -> None:
    mode = "rw" if writable else "ro"
    subprocess.run(
        ["mount", "-o", f"remount,{mode}", str(mount_point)],
        check=True,
        capture_output=True,
    )


def reexec_as_root(arguments: list[str]) -> int:
    """以 root 重新执行自身。

    remount 持久层需要 root，而主服务以 User=dcdw 运行，因此 helper 被调用时
    不具备该权限。这里只在确实需要写持久层时提权，非 overlay 环境完全不触发。

    使用 sudo -n：拿不到免密授权就立即失败，不会挂起等待终端输入密码——helper
    是被主程序 fork 出来的，没有可交互的终端。

    收窄 dcdw 的全局免密 sudo 时，需要保留的最小规则是：
        dcdw ALL=(root) NOPASSWD: /usr/local/libexec/cns-rpi-apply-config
    绝不要放行 `sudo mount *` 或 `sudo overlayroot-chroot *`，两者等价于给 root。
    """
    self_path = Path(__file__).resolve()
    completed = subprocess.run(
        ["sudo", "-n", str(self_path), *arguments], capture_output=True
    )
    sys.stderr.write(completed.stderr.decode("utf-8", "replace"))
    sys.stdout.write(completed.stdout.decode("utf-8", "replace"))
    return completed.returncode


def restore_ownership(target: Path, reference: os.stat_result) -> None:
    """把属主与权限还原成提权前的值。

    以 root 写入会让 config.json 变成 root:root，而主程序以 dcdw 运行且该文件是
    0600——不还原的话主程序下次启动会因权限不足读不到自己的配置。
    """
    os.chown(target, reference.st_uid, reference.st_gid)
    os.chmod(target, stat.S_IMODE(reference.st_mode))


def apply_candidate(
    candidate: Path,
    target: Path,
    allowed_directory: Path = STAGING_DIR,
    allowed_target: Path = CONFIG_PATH,
) -> None:
    """校验固定暂存目录内的候选 JSON，并以原子替换方式提交到固定目标。

    候选文件与目标文件分处两个目录：候选在可写的暂存目录（tmpfs），目标在
    平时只读的配置挂载点。暂存与原子替换始终发生在目标所在目录，两者必须同
    文件系统，rename 才具备原子性。
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

    # 暂存与替换固定发生在目标所在目录，保证 rename 是同文件系统内的原子操作。
    write_directory = allowed_target.parent
    write_target = allowed_target
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
        mount_point = readonly_mount_for(CONFIG_DIR)
        if mount_point is None:
            apply_candidate(candidate, target)
            return 0

        # remount 需要 root，主服务却以 dcdw 运行；提权推迟到确认配置目录确实是
        # 只读挂载之后，普通环境的调用路径完全不涉及 sudo。
        if os.geteuid() != 0:
            return reexec_as_root(arguments)

        previous = os.stat(CONFIG_PATH, follow_symlinks=False)
        _remount(mount_point, writable=True)
        try:
            apply_candidate(candidate, target)
            restore_ownership(CONFIG_PATH, previous)
            # 数据已 fsync 到文件与目录，这里再 sync 一次覆盖底层设备缓存，
            # 之后才允许恢复只读——现场是直接断电，不能依赖延迟回写。
            os.sync()
        finally:
            # 无论写入成功与否都必须恢复只读，否则设备会停在可写状态，
            # 失去断电保护。配置目录是独立文件系统，不受 overlay 钉住，
            # 这一步能够真正成功——这正是不把配置放在根分区上的原因。
            _remount(mount_point, writable=False)
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
