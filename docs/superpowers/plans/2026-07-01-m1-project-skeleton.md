# M1 项目骨架 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 CMake 工程能在树莓派上原生编译出一个空壳可执行文件，并把仓库推送到 GitHub——验证"开发机写代码 → push → RPi git pull → 本地编译运行"这条开发流程本身能跑通。

**Architecture:** 一个最小的 CMake 工程（`CMakeLists.txt` + `src/main.cpp`），不接入任何业务逻辑（UART/MAVLink/MQTT 留给 M2+）。配套一份幂等的环境准备脚本 `scripts/install_deps.sh`，覆盖 RPi 换国内 apt 源和装构建依赖这两步。

**Tech Stack:** C++20, CMake（原生编译，不做交叉编译），目标平台 Raspberry Pi OS 64-bit（Bookworm, ARM64）。

## Global Constraints

- 语言标准：C++20（`docs/V1设计文档.md` §7）
- 构建方式：CMake，在 RPi 上原生编译，不做交叉编译（`docs/V1设计文档.md` §7）
- 编译警告：`-Wall -Wextra`，警告需清零或有明确理由保留（`docs/协作规则.md` §6）
- 提交信息格式：`<type>: <简短中文说明>`，type 用小写英文，说明用简短中文（`docs/协作规则.md` §2）
- 源码注释：Doxygen 风格中文注释，文件头写职责/层级/依赖/禁止事项（`docs/协作规则.md` §3）
- RPi 目标机：`dcdw@192.168.11.4`，Raspberry Pi OS 64-bit（Bookworm），apt 源尚未切换
- GitHub 仓库：公开（Public），账号 `oran9eLi`，仓库名 `cns_rpi`
- apt 镜像：清华大学 TUNA 镜像站（`mirrors.tuna.tsinghua.edu.cn`）

---

### Task 1: RPi 环境准备脚本 `scripts/install_deps.sh`

**Files:**
- Create: `scripts/install_deps.sh`

**Interfaces:**
- Consumes: 无（独立脚本，不依赖仓库内其他代码）
- Produces: 无代码接口；副作用是 RPi 上换好 apt 源并装好 `build-essential`/`cmake`/`git`，供 Task 4 的端到端验证使用

- [ ] **Step 1: 写脚本**

```bash
#!/usr/bin/env bash
# scripts/install_deps.sh
# 树莓派环境准备：切换国内 apt 源（清华 TUNA 镜像站）+ 安装构建依赖。
# 幂等：重复执行不会破坏已有配置（会先备份旧 sources 文件）。
set -euo pipefail

TIMESTAMP="$(date +%Y%m%d%H%M%S)"

echo "==> 备份并切换 /etc/apt/sources.list 为清华 TUNA 镜像"
sudo cp /etc/apt/sources.list "/etc/apt/sources.list.bak.${TIMESTAMP}"
sudo tee /etc/apt/sources.list > /dev/null <<'SOURCES'
deb https://mirrors.tuna.tsinghua.edu.cn/debian/ bookworm main contrib non-free non-free-firmware
deb https://mirrors.tuna.tsinghua.edu.cn/debian/ bookworm-updates main contrib non-free non-free-firmware
deb https://mirrors.tuna.tsinghua.edu.cn/debian-security bookworm-security main contrib non-free non-free-firmware
SOURCES

if [ -f /etc/apt/sources.list.d/raspi.list ]; then
  echo "==> 备份并切换 /etc/apt/sources.list.d/raspi.list 为清华 TUNA 镜像"
  sudo cp /etc/apt/sources.list.d/raspi.list "/etc/apt/sources.list.d/raspi.list.bak.${TIMESTAMP}"
fi
sudo tee /etc/apt/sources.list.d/raspi.list > /dev/null <<'SOURCES'
deb https://mirrors.tuna.tsinghua.edu.cn/raspberrypi/ bookworm main
SOURCES

echo "==> apt update"
sudo apt update

echo "==> 安装构建依赖：build-essential cmake git"
sudo apt install -y build-essential cmake git

echo "==> 完成，版本信息："
g++ --version
cmake --version
git --version
```

- [ ] **Step 2: 赋予可执行权限**

Run: `chmod +x scripts/install_deps.sh`

- [ ] **Step 3: 传到 RPi 上执行**

Run:
```bash
scp scripts/install_deps.sh dcdw@192.168.11.4:/tmp/install_deps.sh
ssh dcdw@192.168.11.4 'chmod +x /tmp/install_deps.sh && /tmp/install_deps.sh'
```

Expected: 输出以 `g++ (Debian ...) 12.2.0` 左右版本号结尾（Bookworm 默认 GCC 12），`cmake version 3.2x.x`，`git version 2.x.x`，过程中 `apt update`/`apt install` 均无报错退出。

- [ ] **Step 4: Commit**

```bash
git add scripts/install_deps.sh
git commit -m "chore: 新增 RPi 环境准备脚本(换源+装构建依赖)"
```

---

### Task 2: CMake 项目骨架 + 空壳可执行文件

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/main.cpp`

**Interfaces:**
- Consumes: 无
- Produces: 可执行目标 `cns_rpi`（构建产物路径 `build/cns_rpi`），后续 M2+ 任务会往 `CMakeLists.txt` 里追加 `add_subdirectory`/新源文件，往 `main.cpp` 里追加初始化调用，但本任务只交付一个能编译运行的空壳

- [ ] **Step 1: 写 `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
project(cns_rpi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)

add_executable(cns_rpi src/main.cpp)
```

- [ ] **Step 2: 写 `src/main.cpp`**

```cpp
/**
 * @file main.cpp
 * @brief 程序入口，组合根。
 *
 * @details
 * M1 阶段只验证构建链路（CMake 能在 RPi 上原生编译出可执行文件），
 * 不接入 UART/MAVLink/MQTT 等业务逻辑，这些从 M2 开始逐步接入本文件。
 */

#include <iostream>

int main() {
  std::cout << "cns_rpi starting (M1 skeleton)" << std::endl;
  return 0;
}
```

- [ ] **Step 3: 本地构建验证（开发机）**

Run:
```bash
cmake -B build -S .
cmake --build build
```

Expected: 两条命令均以 0 退出码结束，无 `-Wall -Wextra` 警告输出，`build/` 下生成可执行文件 `cns_rpi`。

- [ ] **Step 4: 运行验证**

Run: `./build/cns_rpi`
Expected: 输出恰好一行 `cns_rpi starting (M1 skeleton)`

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/main.cpp
git commit -m "build: 建立 CMake 项目骨架与空壳可执行文件"
```

---

### Task 3: GitHub 远程仓库创建并推送

**Files:**
- 无文件改动，仅仓库/远程操作

**Interfaces:**
- Consumes: Task 1、Task 2 产生的本地提交
- Produces: GitHub 远程 `origin`，供 Task 4 从 RPi 端 clone

- [ ] **Step 1: 创建远程仓库并推送**

Run（在仓库根目录 `/home/oran9e/cns_rpi` 下执行）：
```bash
gh repo create cns_rpi --public --source=. --remote=origin --push
```

Expected: 命令输出仓库 URL（形如 `https://github.com/oran9eLi/cns_rpi`），无报错。

- [ ] **Step 2: 验证远程关联和推送结果**

Run: `git remote -v && git log origin/main --oneline -5`
Expected: `origin` 指向 `https://github.com/oran9eLi/cns_rpi.git`（fetch/push 各一行），`git log origin/main` 能看到本地的提交历史（包括 Task 1、Task 2 的 commit 以及仓库初始化的 `chore: 初始化仓库`）。

（本任务无代码需要提交，Step 1 本身就是推送动作。）

---

### Task 4: RPi 端到端验证

**Files:**
- 无仓库内文件改动，在 RPi 上执行验证

**Interfaces:**
- Consumes: Task 1 的环境准备结果、Task 2 的 CMake 工程、Task 3 推送到 GitHub 的代码
- Produces: 无（本任务是 M1 里程碑的最终验收，确认"开发机 push → RPi pull → 本地编译运行"整条链路可用）

- [ ] **Step 1: 在 RPi 上克隆仓库**

Run:
```bash
ssh dcdw@192.168.11.4 'git clone https://github.com/oran9eLi/cns_rpi.git ~/cns_rpi'
```

Expected: 克隆成功，无报错，`~/cns_rpi` 下能看到 `CMakeLists.txt`、`src/main.cpp`、`scripts/install_deps.sh` 等文件。

- [ ] **Step 2: 在 RPi 上原生编译**

Run:
```bash
ssh dcdw@192.168.11.4 'cd ~/cns_rpi && cmake -B build -S . && cmake --build build'
```

Expected: 两条命令均以 0 退出码结束，无编译警告，生成 `~/cns_rpi/build/cns_rpi`。

- [ ] **Step 3: 在 RPi 上运行验证**

Run:
```bash
ssh dcdw@192.168.11.4 '~/cns_rpi/build/cns_rpi'
```

Expected: 输出 `cns_rpi starting (M1 skeleton)`。

到此，M1 里程碑（"CMake 工程能在 RPi 上原生编译出空壳可执行文件，GitHub 仓库建好并推送"）验收完成。后续新机器/复现环境时，直接 `git clone` 仓库后运行仓库内的 `scripts/install_deps.sh`（不用再走 Task 1 的 scp 方式，那是本次仓库还没推送前的引导手段）。

---

## Self-Review

**Spec coverage**：`docs/V1设计文档.md` §10 M1 的两项要求——"CMake 工程能在 RPi 上原生编译出空壳可执行文件"对应 Task 2 + Task 4，"GitHub 仓库建好并推送"对应 Task 3——均有对应任务覆盖。`docs/协作规则.md` 的构建验证（`-Wall -Wextra`）、提交信息格式在每个任务的 commit step 里都落实了。

**Placeholder scan**：所有步骤都是可直接执行的完整命令或完整代码，没有 TBD/待补充。

**Type consistency**：`cns_rpi` 作为 CMake target 名和最终可执行文件名在 Task 2、Task 4 里保持一致；仓库路径 `~/cns_rpi`（RPi 上）与 `/home/oran9e/cns_rpi`（开发机上）不是同一路径但都是同一仓库的本地副本，已在各步骤里明确区分是在哪台机器上执行。
