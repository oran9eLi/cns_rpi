#!/usr/bin/env bash
# 构建并幂等安装 cns_rpi 主程序的配置 helper 与 systemd 服务。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
HELPER_SOURCE="${SCRIPT_DIR}/cns-rpi-apply-config.py"
HELPER_TARGET="/usr/local/libexec/cns-rpi-apply-config"
SERVICE_SOURCE="${REPO_ROOT}/systemd/cns-rpi.service"
SERVICE_TARGET="/etc/systemd/system/cns-rpi.service"
CELLULAR_SERVICE_SOURCE="${REPO_ROOT}/systemd/cellular-dialup.service"
CELLULAR_SERVICE_TARGET="/etc/systemd/system/cellular-dialup.service"
JOURNALD_CONF_SOURCE="${REPO_ROOT}/systemd/journald-cns-rpi.conf"
JOURNALD_CONF_TARGET="/etc/systemd/journald.conf.d/90-cns-rpi.conf"
MOUNT_HELPER_SOURCE="${SCRIPT_DIR}/cns-rpi-mount-config.sh"
MOUNT_HELPER_TARGET="/usr/local/libexec/cns-rpi-mount-config"
CONFIG_MOUNT_SERVICE_SOURCE="${REPO_ROOT}/systemd/cns-rpi-config.service"
CONFIG_MOUNT_SERVICE_TARGET="/etc/systemd/system/cns-rpi-config.service"
CONFIG_DIR="/var/lib/cns-rpi"
CONFIG_PATH="${CONFIG_DIR}/config.json"
LEGACY_CONFIG_PATH="${REPO_ROOT}/config/config.json"
EXPECTED_REPO_ROOT="/home/dcdw/cns_rpi"

echo "===== 检查根文件系统是否可持久写入 ====="
# OverlayFS 生效时根文件系统的写入全部落在内存上层，重启即蒸发：本脚本安装的
# systemd unit、helper 以及 git pull 下来的代码都会消失，而部署过程看起来完全
# 成功——直到下一次重启才暴露。这里直接拒绝运行，不给出"看似成功"的结果。
#
# 该问题曾在 2026-07-20 真实发生过：overlay 生效状态下完成的部署在重启后全部
# 回退，详见 docs/OverlayFS只读根文件系统设计.md。
if [ "$(findmnt -n -o FSTYPE / 2>/dev/null)" = "overlay" ]; then
  cat >&2 <<'GUARD'
错误：检测到 OverlayFS 已启用，根文件系统不可持久写入，拒绝部署。

维护流程（每步之间需要重启）：
  1. sudo raspi-config nonint disable_overlayfs && sudo reboot
  2. 重启后重新执行 ./scripts/deploy.sh
  3. sudo raspi-config nonint enable_overlayfs && sudo reboot

不要在 overlay 生效时用 overlayroot-chroot 绕过本检查执行完整部署：
构建产物、服务状态和挂载关系都不在 chroot 视图内，结果不可靠。
GUARD
  exit 1
fi
echo "  - 根文件系统可持久写入"

echo "===== 验证 sudo 权限 ====="
if [ "$(id -u)" -eq 0 ]; then
  echo "错误：请使用 dcdw 普通用户执行本脚本，不要执行 sudo ./scripts/deploy.sh。" >&2
  exit 1
fi
if [ "$(id -un)" != "dcdw" ] || [ "${REPO_ROOT}" != "${EXPECTED_REPO_ROOT}" ]; then
  echo "错误：生产部署只能由 dcdw 在 ${EXPECTED_REPO_ROOT} 中执行。" >&2
  exit 1
fi
if sudo -n true 2>/dev/null; then
  echo "  - sudo 免密授权可用"
else
  sudo -v
fi

echo "===== 构建 cns_rpi ====="
cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build"
cmake --build "${REPO_ROOT}/build"

install_if_changed() {
  local source="$1"
  local target="$2"
  local mode="$3"
  sudo install -D -o root -g root -m "${mode}" "${source}" "${target}"
  echo "  - 已收敛 ${target} 的内容、所有者和权限"
}

echo "===== 安装配置 helper 和 systemd 服务 ====="
install_if_changed "${HELPER_SOURCE}" "${HELPER_TARGET}" 0755
install_if_changed "${MOUNT_HELPER_SOURCE}" "${MOUNT_HELPER_TARGET}" 0755
install_if_changed "${SERVICE_SOURCE}" "${SERVICE_TARGET}" 0644
install_if_changed "${CONFIG_MOUNT_SERVICE_SOURCE}" "${CONFIG_MOUNT_SERVICE_TARGET}" 0644
install_if_changed "${CELLULAR_SERVICE_SOURCE}" "${CELLULAR_SERVICE_TARGET}" 0644

echo "===== 安装 journald 内存化配置 ====="
# 只在内容真正变化时重启 journald：重启会切断 journalctl -f 之类的现场排查，
# 每次部署都无条件重启没有必要。
if sudo cmp -s "${JOURNALD_CONF_SOURCE}" "${JOURNALD_CONF_TARGET}"; then
  echo "  - ${JOURNALD_CONF_TARGET} 已是最新，跳过重启 journald"
else
  install_if_changed "${JOURNALD_CONF_SOURCE}" "${JOURNALD_CONF_TARGET}" 0644
  sudo systemctl restart systemd-journald
  echo "  - 已重启 systemd-journald 使日志容量上限生效"
fi

echo "===== 加载并启用 systemd 服务 ====="
sudo systemctl daemon-reload
sudo systemctl enable cellular-dialup.service
sudo systemctl start --no-block cellular-dialup.service
# 配置卷存在时才启用挂载服务：尚未建立持久化卷的设备（开发机、迁移过渡期）
# 直接用普通目录，不应因为多了这个单元而起不来。
if [ -f /boot/firmware/cns-config.img ]; then
  sudo systemctl enable cns-rpi-config.service
  sudo systemctl start cns-rpi-config.service
  echo "  - 配置持久化卷已挂载"
else
  echo "  - 未发现 /boot/firmware/cns-config.img，配置目录按普通目录使用"
fi

# 配置检查必须放在挂载之后：卷挂上前 ${CONFIG_DIR} 是空的，
# 提前检查会误判为"缺少配置"并把仓库里的旧配置迁进去，
# 随后又被挂载遮蔽，留下两份互相矛盾的配置。
echo "===== 检查现场配置 ====="
sudo install -d -o dcdw -g dcdw -m 0755 "${CONFIG_DIR}"
if [ -f "${CONFIG_PATH}" ]; then
  echo "  - 现场配置就位：${CONFIG_PATH}"
  if [ -f "${LEGACY_CONFIG_PATH}" ]; then
    echo "  - 警告：旧位置仍存在 ${LEGACY_CONFIG_PATH}，实际生效的是 ${CONFIG_PATH}" >&2
  fi
elif [ -f "${LEGACY_CONFIG_PATH}" ]; then
  echo "  - 检测到旧位置配置，迁移到 ${CONFIG_PATH}"
  sudo install -o dcdw -g dcdw -m 0600 "${LEGACY_CONFIG_PATH}" "${CONFIG_PATH}"
  # 旧文件改名保留而不删除：迁移出问题时还能回退，且避免两处配置并存造成困惑。
  mv "${LEGACY_CONFIG_PATH}" "${LEGACY_CONFIG_PATH}.migrated"
  echo "  - 旧配置已保留为 ${LEGACY_CONFIG_PATH}.migrated"
else
  echo "错误：缺少现场配置 ${CONFIG_PATH}" >&2
  echo "请先根据 ${REPO_ROOT}/config/config.example.json 创建，脚本不会自动生成。" >&2
  exit 1
fi

sudo systemctl enable cns-rpi.service
if sudo systemctl is-active --quiet cns-rpi.service; then
  sudo systemctl restart cns-rpi.service
else
  sudo systemctl start cns-rpi.service
fi

sudo systemctl --no-pager --full status cns-rpi.service
echo "===== cns_rpi 部署完成 ====="
