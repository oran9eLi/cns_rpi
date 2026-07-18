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
CONFIG_PATH="${REPO_ROOT}/config/config.json"
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

if [ ! -f "${CONFIG_PATH}" ]; then
  echo "错误：缺少现场配置 ${CONFIG_PATH}" >&2
  echo "请先根据 config/config.example.json 创建，脚本不会自动覆盖现场配置。" >&2
  exit 1
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
install_if_changed "${SERVICE_SOURCE}" "${SERVICE_TARGET}" 0644
install_if_changed "${CELLULAR_SERVICE_SOURCE}" "${CELLULAR_SERVICE_TARGET}" 0644

echo "===== 加载并启用 systemd 服务 ====="
sudo systemctl daemon-reload
sudo systemctl enable cellular-dialup.service
sudo systemctl start --no-block cellular-dialup.service
sudo systemctl enable cns-rpi.service
if sudo systemctl is-active --quiet cns-rpi.service; then
  sudo systemctl restart cns-rpi.service
else
  sudo systemctl start cns-rpi.service
fi

sudo systemctl --no-pager --full status cns-rpi.service
echo "===== cns_rpi 部署完成 ====="
