# CNS RPi

CNS（通信/导航/监视）实训箱的树莓派端数据汇聚与回传节点。

本仓库负责双向数据链路：上行从 STM32 主控经 UART 收 MAVLink 遥测与身份数据，解析后重组为 JSON，通过 MQTT 发布给硬件部服务器；下行接收管控中心等来源下发的控制命令，转发编码为 MAVLink 命令帧发给 STM32 执行。STM32 固件、管理中心服务端不在本仓库范围内。

## 当前状态

M1-M6 已完成：UART/MAVLink 双向收发、遥测与身份解码、JSON 序列化、MQTT 遥测与注册、运行时配置命令、飞控命令下行均已接入。STM32 串口支持按合法 MAVLink 帧自动发现，USB 设备号变化或运行中插拔后可自动恢复；树莓派 ARM64 已完成 26 项自动化测试，并完成串口拔插恢复和真实 `COMMAND_ACK` 联调。M7 已完成独立有界日志、主程序/拨号服务和幂等部署脚本，OverlayFS、journald 限额和 systemd watchdog 仍待实施。完整状态见 `docs/V1设计文档.md` 第 10 节。

首次在新 RPi 上搭建环境：

```bash
# 第一次 clone 时全局 git 镜像重写还没配置(那是 install_deps.sh 干的事，而脚本本身在仓库里)，
# 所以必须显式带镜像前缀，不能用裸的 https://github.com/... （会因为 RPi 直连 GitHub 网络问题卡住）
git clone https://ghfast.top/https://github.com/oran9eLi/cns_rpi.git cns_rpi
cd cns_rpi
./scripts/install_deps.sh   # 换清华TUNA apt源 + 装构建依赖 + 配置git全局镜像重写
cmake -B build -S .
cmake --build build
./build/cns_rpi
```

跑完 `install_deps.sh` 之后，git 全局重写已经生效，仓库的 `origin` 也可以放心设回裸的 `https://github.com/...`（`git remote set-url origin https://github.com/oran9eLi/cns_rpi.git`），后续 `git pull`/`git clone` 写裸 URL 就行，不用再带镜像前缀。

## 目标平台

- 硬件：Raspberry Pi 5
- 系统：Raspberry Pi OS 64-bit（trixie / Debian 13），ARM64
- 语言：C++23，CMake 原生编译（不做交叉编译）

## 开发入口

| 文档 | 用途 |
|---|---|
| `docs/V1设计文档.md` | V1 架构、协议对接范围、身份策略、技术选型、里程碑计划 |
| `docs/协作规则.md` | 分支策略、提交信息规范、注释规范、变更记录、构建验证要求 |

新增功能前先看设计文档确认范围，提交代码前按协作规则里的构建验证要求自测。

## 开发方式

代码在开发机上写，push 到仓库，树莓派 `git pull` 后本地编译运行。树莓派不作为主开发机。

**网络说明**：RPi 直连 GitHub 不稳定（实测 ping 丢包 66%、curl 连接超时），`install_deps.sh` 会给 RPi 配一条全局 git 配置，把所有 `https://github.com/` 的访问透明重写到镜像代理 `ghfast.top`（`git config --global url."https://ghfast.top/https://github.com/".insteadOf "https://github.com/"`）。这是单点依赖——如果这个镜像站失效，RPi 上的 `git pull`/`clone` 会报错，取消这条重写：`git config --global --unset url."https://ghfast.top/https://github.com/".insteadOf`。
