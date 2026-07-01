#!/usr/bin/env bash
# scripts/install_deps.sh
# 树莓派环境准备：切换国内 apt 源（清华 TUNA 镜像站）+ 安装构建依赖。
# 目标系统：Raspberry Pi OS 64-bit / Debian 13 (trixie)，apt 源为 deb822 格式
# （/etc/apt/sources.list.d/*.sources），传统 /etc/apt/sources.list 存在但为空、不生效，不动它。
# 幂等：重复执行不会破坏已有配置（会先备份旧 .sources 文件）。
set -euo pipefail

TIMESTAMP="$(date +%Y%m%d%H%M%S)"

# 只在目标文件内容和期望内容不一致时才备份+覆盖，避免重复执行时堆积无意义的 .bak 文件。
# 用法：write_sources_if_changed <目标文件路径> <<'SOURCES' ... SOURCES
write_sources_if_changed() {
  local dest="$1"
  local tmp
  tmp="$(mktemp)"
  cat > "$tmp"
  if [ -f "$dest" ] && cmp -s "$tmp" "$dest"; then
    echo "    $dest 内容已是期望状态，跳过"
    rm -f "$tmp"
    return 0
  fi
  if [ -f "$dest" ]; then
    sudo cp "$dest" "${dest}.bak.${TIMESTAMP}"
  fi
  sudo cp "$tmp" "$dest"
  rm -f "$tmp"
}

echo "==> 切换 /etc/apt/sources.list.d/debian.sources 为清华 TUNA 镜像"
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
  echo "==> 切换 /etc/apt/sources.list.d/raspi.sources 为清华 TUNA 镜像"
  write_sources_if_changed /etc/apt/sources.list.d/raspi.sources <<'SOURCES'
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

echo "==> 配置 git 全局 URL 重写，透明代理 github.com（国内直连 GitHub 不稳定，实测丢包/超时）"
git config --global url."https://ghfast.top/https://github.com/".insteadOf "https://github.com/"

echo "==> 完成，版本信息："
g++ --version
cmake --version
git --version
