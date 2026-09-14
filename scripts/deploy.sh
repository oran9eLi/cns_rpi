#!/usr/bin/env bash
# V1.0：构建、迁移现场配置，幂等安装并启动两个常驻服务。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
EXPECTED_REPO_ROOT="/home/dcdw/cns_rpi"
CONFIG_DIR="/var/lib/cns-rpi"
CONFIG_PATH="${CONFIG_DIR}/config.json"
LEGACY_CONFIG_PATH="${REPO_ROOT}/config/config.json"
RUNTIME_DIR="/run/cns-rpi"
HELPER_TARGET="/usr/local/libexec/cns-rpi-apply-config"

echo "===== 检查部署前置条件 ====="
if [ "$(findmnt -n -o FSTYPE /)" = "overlay" ]; then
  echo "错误：V1.0 不支持 OverlayFS。请备份现场配置、停用 OverlayFS 并重启后部署。" >&2
  exit 1
fi
# 不自动卸载旧卷，避免读取挂载点下被遮蔽的旧配置。
if mountpoint -q "${CONFIG_DIR}" || [ -f /etc/systemd/system/cns-rpi-config.service ]; then
  echo "错误：检测到旧配置卷或挂载服务。请备份并迁移为普通配置目录，参见 docs/新设备部署手册.md。" >&2
  exit 1
fi
if [ "$(id -u)" -eq 0 ]; then
  echo "错误：请使用 dcdw 普通用户执行本脚本，不要执行 sudo ./scripts/deploy.sh。" >&2
  exit 1
fi
if [ "$(id -un)" != "dcdw" ] || [ "${REPO_ROOT}" != "${EXPECTED_REPO_ROOT}" ]; then
  echo "错误：生产部署只能由 dcdw 在 ${EXPECTED_REPO_ROOT} 中执行。" >&2
  exit 1
fi
if [ ! -f "${CONFIG_PATH}" ] && [ ! -f "${LEGACY_CONFIG_PATH}" ]; then
  echo "错误：请先根据 config/config.example.json 创建 config/config.json。" >&2
  exit 1
fi
if sudo -n true 2>/dev/null; then
  echo "  - sudo 免密授权可用"
else
  sudo -v
fi

echo "===== 构建 cns_rpi ====="
cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "${REPO_ROOT}/build" -j2

install_if_changed() {
  sudo install -D -o root -g root -m "$3" "$1" "$2"
  echo "  - 已收敛 $2 的内容、所有者和权限"
}

echo "===== 准备并校验现场配置 ====="
# 先在临时副本迁移并进行完整参数校验，错误配置不得写入持久路径。
PREFLIGHT_CONFIG="$(mktemp)"
trap 'rm -f -- "${PREFLIGHT_CONFIG}"' EXIT
if [ -f "${CONFIG_PATH}" ]; then
  cp -- "${CONFIG_PATH}" "${PREFLIGHT_CONFIG}"
else
  cp -- "${LEGACY_CONFIG_PATH}" "${PREFLIGHT_CONFIG}"
fi
python3 "${SCRIPT_DIR}/migrate_telemetry_config.py" "${PREFLIGHT_CONFIG}"
"${REPO_ROOT}/build/cns_rpi" --check-config "${PREFLIGHT_CONFIG}"
python3 -c 'import sys; sys.path.insert(0, sys.argv[1]); from cellular_dialup import load_cellular_config; load_cellular_config(sys.argv[2])' \
  "${SCRIPT_DIR}" "${PREFLIGHT_CONFIG}"
sudo install -d -o dcdw -g dcdw -m 0755 "${CONFIG_DIR}"
sudo install -d -o dcdw -g dcdw -m 0700 "${RUNTIME_DIR}"
install_if_changed "${SCRIPT_DIR}/cns-rpi-apply-config.py" "${HELPER_TARGET}" 0755
if [ ! -f "${CONFIG_PATH}" ]; then
  sudo install -o dcdw -g dcdw -m 0600 "${PREFLIGHT_CONFIG}" "${CONFIG_PATH}"
  INITIAL_CONFIG=1
else
  INITIAL_CONFIG=0
  echo "  - 保留现场配置：${CONFIG_PATH}"
fi
bash "${SCRIPT_DIR}/migrate_deployed_telemetry_config.sh" \
  "${SCRIPT_DIR}/migrate_telemetry_config.py" "${HELPER_TARGET}" \
  "${CONFIG_PATH}" "${RUNTIME_DIR}"
"${REPO_ROOT}/build/cns_rpi" --check-config "${CONFIG_PATH}"
"${REPO_ROOT}/build/cns_rpi" --version
if [ "${INITIAL_CONFIG}" -eq 1 ]; then
  mv -- "${LEGACY_CONFIG_PATH}" "${LEGACY_CONFIG_PATH}.migrated"
fi

echo "===== 安装配置 helper 和 systemd 服务 ====="
install_if_changed "${REPO_ROOT}/systemd/cns-rpi.service" /etc/systemd/system/cns-rpi.service 0644
install_if_changed "${REPO_ROOT}/systemd/cellular-dialup.service" /etc/systemd/system/cellular-dialup.service 0644

SWAP_TARGET=/etc/rpi/swap.conf.d/50-cns-rpi.conf
if ! sudo cmp -s "${REPO_ROOT}/systemd/swap-cns-rpi.conf" "${SWAP_TARGET}"; then
  install_if_changed "${REPO_ROOT}/systemd/swap-cns-rpi.conf" "${SWAP_TARGET}" 0644
  echo "  - 纯 zram swap 配置将在重启后生效"
fi
JOURNAL_TARGET=/etc/systemd/journald.conf.d/90-cns-rpi.conf
if ! sudo cmp -s "${REPO_ROOT}/systemd/journald-cns-rpi.conf" "${JOURNAL_TARGET}"; then
  install_if_changed "${REPO_ROOT}/systemd/journald-cns-rpi.conf" "${JOURNAL_TARGET}" 0644
  sudo systemctl restart systemd-journald
fi

echo "===== 加载并启用 systemd 服务 ====="
sudo systemctl daemon-reload
sudo systemctl enable cns-rpi.service
sudo systemctl enable cellular-dialup.service
# 两项服务互不依赖；5G 无卡不阻止主程序通过其他网络运行。
if sudo systemctl is-active --quiet cns-rpi.service; then
  sudo systemctl restart cns-rpi.service
else
  sudo systemctl start cns-rpi.service
fi
if sudo systemctl is-active --quiet cellular-dialup.service; then
  sudo systemctl restart cellular-dialup.service
else
  sudo systemctl start cellular-dialup.service
fi
sudo systemctl --no-pager --full status cns-rpi.service
echo "===== cns_rpi 部署完成 ====="
