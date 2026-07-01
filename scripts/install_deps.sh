#!/usr/bin/env bash
# scripts/install_deps.sh
# 树莓派环境准备：切换国内 apt 源（清华 TUNA 镜像站）+ 安装构建依赖 + 配置 git 镜像重写。
# 目标系统：Raspberry Pi OS 64-bit / Debian 13 (trixie)，apt 源为 deb822 格式
# （/etc/apt/sources.list.d/*.sources），传统 /etc/apt/sources.list 存在但为空、不生效，不动它。
# 幂等：重复执行不会破坏已有配置，也不会重复堆积备份文件（内容不变就跳过）。
set -euo pipefail

TOTAL_STEPS=4
TIMESTAMP="$(date +%Y%m%d%H%M%S)"

step() {
  echo
  echo "===== [$1/${TOTAL_STEPS}] $2 ====="
}

# 只在目标文件内容和期望内容不一致时才备份+覆盖，避免重复执行时堆积无意义的 .bak 文件。
# 用法：write_sources_if_changed <目标文件路径> <<'SOURCES' ... SOURCES
write_sources_if_changed() {
  local dest="$1"
  local tmp
  tmp="$(mktemp)"
  cat > "$tmp"
  if [ -f "$dest" ] && cmp -s "$tmp" "$dest"; then
    echo "  - $dest 内容已是期望状态，跳过"
    rm -f "$tmp"
    return 0
  fi
  if [ -f "$dest" ]; then
    echo "  - 备份原文件到 ${dest}.bak.${TIMESTAMP}"
    sudo cp "$dest" "${dest}.bak.${TIMESTAMP}"
  fi
  sudo cp "$tmp" "$dest"
  rm -f "$tmp"
  echo "  - 已写入 $dest"
}

cat <<'BANNER'
本脚本会在这台树莓派上做以下几件事：
  1. 把 apt 软件源换成清华 TUNA 镜像站（国内直连官方源慢，甚至连不上）
  2. 装 C++ 构建工具链：build-essential / cmake / git
  3. 配置 git 全局重写，把 github.com 的访问透明转到国内可达的镜像代理
     （RPi 直连 GitHub 实测丢包/超时，不然以后 git pull/clone 会卡住）
  4. 打印版本信息，供你确认安装结果
只会修改 apt 源配置和 git 全局配置这两处系统状态，不改动本仓库以外的其他文件。
BANNER

step 1 "切换 apt 源为清华 TUNA 镜像（原因：国内直连官方源慢/不稳定）"

write_sources_if_changed /etc/apt/sources.list.d/debian.sources <<'SOURCES'
Types: deb
URIs: https://mirrors.tuna.tsinghua.edu.cn/debian/
Suites: trixie trixie-updates
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp

Types: deb
URIs: https://mirrors.tuna.tsinghua.edu.cn/debian-security/
Suites: trixie-security
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.pgp
SOURCES

if [ -f /etc/apt/sources.list.d/raspi.sources ]; then
  write_sources_if_changed /etc/apt/sources.list.d/raspi.sources <<'SOURCES'
Types: deb
URIs: https://mirrors.tuna.tsinghua.edu.cn/raspberrypi/
Suites: trixie
Components: main
Signed-By: /usr/share/keyrings/raspberrypi-archive-keyring.pgp
SOURCES
else
  echo "  - 未发现 /etc/apt/sources.list.d/raspi.sources，跳过（这台机器可能不需要树莓派专属源）"
fi

step 2 "安装构建依赖：build-essential cmake git（先 apt update 刷新索引）"
sudo apt update
sudo apt install -y build-essential cmake git

step 3 "配置 git 全局镜像重写（原因：RPi 直连 GitHub 网络不通/不稳定）"
git config --global url."https://ghfast.top/https://github.com/".insteadOf "https://github.com/"
echo "  - 已生效：以后对 https://github.com/ 的 git 操作会透明走 https://ghfast.top/ 代理"
echo "  - 如果这个镜像失效，取消方法：git config --global --unset url.\"https://ghfast.top/https://github.com/\".insteadOf"

step 4 "版本信息（确认安装结果）"
g++ --version
cmake --version
git --version

echo
echo "===== 环境准备完成 ====="
