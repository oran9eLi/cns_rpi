#!/usr/bin/env bash
# 创建配置持久化卷，并把现有现场配置迁进去。
#
# 只需在一台设备上执行一次。之后由 cns-rpi-config.service 负责开机只读挂载，
# 由 scripts/deploy.sh 负责安装那个服务。
#
# 为什么配置要单独放一个文件系统，而不是直接放在根分区上：
#   OverlayFS 启用后，根分区是 overlay 的 lowerdir，被内核钉住，写完之后无法
#   remount 回只读（EBUSY），断电保护会失效。独立文件系统不参与 overlay，
#   读写状态完全自主，写配置时切可写、写完切回只读才能真正成功。
#   详见 docs/OverlayFS只读根文件系统设计.md。
#
# 为什么镜像放在 /boot/firmware：
#   那是独立的 vfat 分区，不受 overlay 影响，且镜像文件大小固定，
#   写入只改数据块、不改 vfat 元数据。镜像内部是 ext4，原子 rename、fsync
#   和属主权限语义都保留——这些正是 vfat 给不了的。
set -euo pipefail

IMAGE="/boot/firmware/cns-config.img"
MOUNT_POINT="/var/lib/cns-rpi"
SIZE_MIB="${CNS_CONFIG_VOLUME_MIB:-16}"
OWNER="dcdw"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LEGACY_CONFIG="${REPO_ROOT}/config/config.json"

if [ "$(id -u)" -eq 0 ]; then
  echo "错误：请以 ${OWNER} 普通用户执行，不要用 sudo 直接跑本脚本。" >&2
  exit 1
fi

# 本脚本要写 /var/lib/cns-rpi 所在的根分区（建挂载点），overlay 生效时那是内存层。
if [ "$(findmnt -n -o FSTYPE / 2>/dev/null)" = "overlay" ]; then
  cat >&2 <<'GUARD'
错误：检测到 OverlayFS 已启用，挂载点会建在内存层、重启即失，拒绝执行。

请先关闭 overlay 再执行：
  sudo raspi-config nonint disable_overlayfs && sudo reboot
GUARD
  exit 1
fi

if [ -f "${IMAGE}" ]; then
  echo "错误：${IMAGE} 已存在，拒绝覆盖。" >&2
  echo "该卷可能已承载现场配置；确认不再需要后请手工删除，再重新执行本脚本。" >&2
  exit 1
fi

if mountpoint -q "${MOUNT_POINT}"; then
  echo "错误：${MOUNT_POINT} 当前已是挂载点，请先 sudo systemctl stop cns-rpi-config.service。" >&2
  exit 1
fi

# 找一份用来初始化的配置：优先已在新路径的，其次仓库里的旧位置配置。
if [ -f "${MOUNT_POINT}/config.json" ]; then
  SOURCE_CONFIG="${MOUNT_POINT}/config.json"
elif [ -f "${LEGACY_CONFIG}" ]; then
  SOURCE_CONFIG="${LEGACY_CONFIG}"
else
  echo "错误：找不到可用于初始化的现场配置。" >&2
  echo "请先根据 ${REPO_ROOT}/config/config.example.json 创建 ${LEGACY_CONFIG}。" >&2
  exit 1
fi
python3 -m json.tool "${SOURCE_CONFIG}" >/dev/null

echo "===== 创建配置持久化卷 ====="
echo "  - 镜像:   ${IMAGE} (${SIZE_MIB} MiB, ext4)"
echo "  - 挂载点: ${MOUNT_POINT}"
echo "  - 初始配置来源: ${SOURCE_CONFIG}"

sudo dd if=/dev/zero of="${IMAGE}" bs=1M count="${SIZE_MIB}" status=none
sudo mkfs.ext4 -q -F -L cns-config "${IMAGE}"

STAGE="$(mktemp -d)"
# 用可写方式建立 loop 设备：mount -o loop 配 ro 会把 loop 设备本身标记为写保护，
# 之后 helper 的 remount,rw 会在设备层被拒。
LOOP="$(sudo losetup --find --show "${IMAGE}")"
cleanup() {
  sudo umount "${STAGE}" 2>/dev/null || true
  sudo losetup -d "${LOOP}" 2>/dev/null || true
  rmdir "${STAGE}" 2>/dev/null || true
}
trap cleanup EXIT

sudo mount "${LOOP}" "${STAGE}"
sudo install -o "${OWNER}" -g "${OWNER}" -m 0600 "${SOURCE_CONFIG}" "${STAGE}/config.json"
sudo sync
echo "  - 已写入 config.json"

cleanup
trap - EXIT

# 挂载点必须存在于持久层：overlay 启用后新建的目录只在内存层，重启就没了，
# 挂载会失败，主服务会因读不到配置起不来。
sudo install -d -o "${OWNER}" -g "${OWNER}" -m 0755 "${MOUNT_POINT}"

echo "===== 配置持久化卷创建完成 ====="
echo "下一步：./scripts/deploy.sh   （会安装并启用 cns-rpi-config.service 完成挂载）"
