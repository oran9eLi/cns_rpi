#!/usr/bin/env bash
# 把配置持久化卷以只读方式挂到 /var/lib/cns-rpi。
#
# 为什么不用 fstab：
#   `mount -o ro,loop <img>` 会把 loop 设备本身也标记为写保护（losetup RO=1），
#   之后 helper 再执行 `mount -o remount,rw` 会在设备层被拒，配置永远写不进去。
#   必须先以可写方式建立 loop 设备，再把文件系统挂成 ro。
#
# 为什么用 oneshot 服务而不是 .mount 单元：
#   fstab / .mount 挂载失败可能把系统拖进 emergency shell，现场设备一旦如此
#   就只能物理接触才能恢复。做成独立服务后，挂载失败只会让 cns-rpi.service
#   起不来，系统本身照常启动、SSH 可用，还能远程排查。
set -euo pipefail

IMAGE="/boot/firmware/cns-config.img"
MOUNT_POINT="/var/lib/cns-rpi"

case "${1:-mount}" in
mount)
  if findmnt -n "${MOUNT_POINT}" >/dev/null 2>&1; then
    echo "配置卷已挂载于 ${MOUNT_POINT}，跳过"
    exit 0
  fi
  if [ ! -f "${IMAGE}" ]; then
    # 尚未建立持久化卷的设备（开发机、迁移过渡期）按普通目录使用。
    # 这里成功退出，否则 cns-rpi.service 的 Requires= 会连带把主服务拖住。
    # 真正的风险（配置缺失）由主程序读不到 config.json 时明确报错兜底。
    echo "未发现 ${IMAGE}，${MOUNT_POINT} 按普通目录使用"
    exit 0
  fi
  mkdir -p "${MOUNT_POINT}"
  # 复用已有的 loop 关联，避免重复 attach 同一个镜像。
  loop="$(losetup -j "${IMAGE}" -O NAME -n | head -1 | tr -d ' ')"
  if [ -z "${loop}" ]; then
    loop="$(losetup --find --show "${IMAGE}")"
  fi
  mount -o ro "${loop}" "${MOUNT_POINT}"
  echo "配置卷已只读挂载：${loop} -> ${MOUNT_POINT}"
  ;;
unmount)
  if findmnt -n "${MOUNT_POINT}" >/dev/null 2>&1; then
    umount "${MOUNT_POINT}"
  fi
  loop="$(losetup -j "${IMAGE}" -O NAME -n | head -1 | tr -d ' ')"
  if [ -n "${loop}" ]; then
    losetup -d "${loop}"
  fi
  echo "配置卷已卸载"
  ;;
*)
  echo "用法: $(basename "$0") [mount|unmount]" >&2
  exit 2
  ;;
esac
