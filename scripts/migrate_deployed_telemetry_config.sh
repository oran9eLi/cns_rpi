#!/usr/bin/env bash
# 在可写运行目录生成迁移候选，再交给配置 helper 应用到现场配置。
set -euo pipefail

if [ "$#" -ne 4 ]; then
  echo "用法：migrate_deployed_telemetry_config.sh <迁移脚本> <配置helper> <配置文件> <暂存目录>" >&2
  exit 2
fi

MIGRATOR="$1"
APPLY_HELPER="$2"
CONFIG_PATH="$3"
STAGING_DIR="$4"

CANDIDATE="$(mktemp "${STAGING_DIR}/.config.json.tmp.XXXXXX")"
cleanup() {
  rm -f -- "${CANDIDATE}"
}
trap cleanup EXIT

cp -- "${CONFIG_PATH}" "${CANDIDATE}"
python3 "${MIGRATOR}" "${CANDIDATE}"

if cmp -s -- "${CANDIDATE}" "${CONFIG_PATH}"; then
  echo "  - 现场配置已经是快照/实时通道结构"
else
  "${APPLY_HELPER}" "${CANDIDATE}" "${CONFIG_PATH}"
  echo "  - 已通过配置 helper 原子应用遥测通道迁移"
fi
