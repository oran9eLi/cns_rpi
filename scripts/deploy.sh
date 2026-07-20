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

echo "===== 检查现场配置 ====="
# 运行时配置已从 git 工作树迁到 /var/lib/cns-rpi：它是可变的现场状态，
# 且该目录要作为独立文件系统的挂载点（只读根文件系统方案）。
if [ ! -f "${CONFIG_PATH}" ]; then
  if [ -f "${LEGACY_CONFIG_PATH}" ]; then
    echo "  - 检测到旧位置配置，迁移到 ${CONFIG_PATH}"
    sudo install -d -o dcdw -g dcdw -m 0755 "${CONFIG_DIR}"
    sudo install -o dcdw -g dcdw -m 0600 "${LEGACY_CONFIG_PATH}" "${CONFIG_PATH}"
    # 旧文件改名保留而不删除：迁移出问题时还能回退，且避免两处配置并存造成困惑。
    mv "${LEGACY_CONFIG_PATH}" "${LEGACY_CONFIG_PATH}.migrated"
    echo "  - 旧配置已保留为 ${LEGACY_CONFIG_PATH}.migrated"
  else
    echo "错误：缺少现场配置 ${CONFIG_PATH}" >&2
    echo "请先根据 ${REPO_ROOT}/config/config.example.json 创建，脚本不会自动生成。" >&2
    exit 1
  fi
else
  sudo install -d -o dcdw -g dcdw -m 0755 "${CONFIG_DIR}"
  if [ -f "${LEGACY_CONFIG_PATH}" ]; then
    echo "  - 警告：旧位置仍存在 ${LEGACY_CONFIG_PATH}，实际生效的是 ${CONFIG_PATH}" >&2
  fi
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
sudo systemctl enable cns-rpi.service
if sudo systemctl is-active --quiet cns-rpi.service; then
  sudo systemctl restart cns-rpi.service
else
  sudo systemctl start cns-rpi.service
fi

sudo systemctl --no-pager --full status cns-rpi.service
echo "===== cns_rpi 部署完成 ====="
