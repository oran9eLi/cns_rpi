#!/usr/bin/env bash
# scripts/install_deps.sh
# 树莓派环境准备：切换国内 apt 源（清华 TUNA 镜像站）+ 安装构建依赖。
# 目标系统：Raspberry Pi OS 64-bit / Debian 13 (trixie)，apt 源为 deb822 格式
# （/etc/apt/sources.list.d/*.sources），传统 /etc/apt/sources.list 存在但为空、不生效，不动它。
# 幂等：重复执行不会破坏已有配置（会先备份旧 .sources 文件）。
set -euo pipefail

TIMESTAMP="$(date +%Y%m%d%H%M%S)"

echo "==> 备份并切换 /etc/apt/sources.list.d/debian.sources 为清华 TUNA 镜像"
sudo cp /etc/apt/sources.list.d/debian.sources "/etc/apt/sources.list.d/debian.sources.bak.${TIMESTAMP}"
sudo tee /etc/apt/sources.list.d/debian.sources > /dev/null <<'SOURCES'
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
  echo "==> 备份并切换 /etc/apt/sources.list.d/raspi.sources 为清华 TUNA 镜像"
  sudo cp /etc/apt/sources.list.d/raspi.sources "/etc/apt/sources.list.d/raspi.sources.bak.${TIMESTAMP}"
  sudo tee /etc/apt/sources.list.d/raspi.sources > /dev/null <<'SOURCES'
Types: deb
URIs: https://mirrors.tuna.tsinghua.edu.cn/raspberrypi/
Suites: trixie
Components: main
Signed-By: /usr/share/keyrings/raspberrypi-archive-keyring.pgp
SOURCES
fi

echo "==> apt update"
sudo apt update

echo "==> 安装构建依赖：build-essential cmake git"
sudo apt install -y build-essential cmake git

echo "==> 完成，版本信息："
g++ --version
cmake --version
git --version
