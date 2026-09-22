# CNS RPi

树莓派端 MAVLink 数据汇聚与 MQTT 转发程序。单台树莓派同一时间连接一台 CNS 主控箱或 PX4 飞控，负责身份获取、遥测解析与上报；主控箱下行命令经 MAVLink 转发并回传执行结果。固件、MQTT broker 和服务器数据库、路由程序不属于本仓库。

## V1.0 发布范围

保留当前主控箱与 PX4 的统一身份、已实现的 MAVLink 消息解析及高低频遥测发布。身份来自受控设备的 `OPEN_DRONE_ID_BASIC_ID.uas_id`，树莓派不提供设备业务身份。PX4 支持范围以现有解析实现为准，不代表全部消息或完整飞控控制支持；主控箱私有命令不适用于 PX4。

部署使用普通可写根文件系统，现场配置保存在 `/var/lib/cns-rpi/config.json`。V1.0 不使用 OverlayFS 或独立配置卷，不新增 MQTT TLS。主程序和 5G 守护服务均由 systemd 开机自启动，保留自动恢复、有界日志与主程序 watchdog。

发布收尾状态见 [发布准备清单](docs/V1.0发布准备清单.md)，V1.0 基线能力与验收边界见 [V1.0.0 发布说明](docs/V1.0发布说明.md)，当前修订内容见 [V1.0.1 发布说明](docs/V1.0.1发布说明.md)。

## 新设备部署

目标为 Raspberry Pi 5、Raspberry Pi OS 64-bit（trixie / Debian 13）、用户 `dcdw`，仓库路径固定为 `/home/dcdw/cns_rpi`。用户须具备 sudo 权限；未配置免密 sudo 时，交互执行脚本会请求密码。

```bash
git clone https://ghfast.top/https://github.com/oran9eLi/cns_rpi.git /home/dcdw/cns_rpi
cd /home/dcdw/cns_rpi
cp config/config.example.json config/config.json
nano config/config.json
./scripts/install_deps.sh
```

先确认现场 MQTT 地址、APN、串口自动发现、上报周期及 `identity.binding_file`，再执行脚本。`install_deps.sh` 安装依赖并调用 `deploy.sh` 构建、初始化配置、安装 helper 和两个常驻服务。已有现场配置会保留，不用仓库样例覆盖。完整步骤与旧只读设备迁移边界见 [新设备部署手册](docs/新设备部署手册.md)。

```bash
systemctl status cns-rpi.service cellular-dialup.service --no-pager
journalctl -u cns-rpi.service -u cellular-dialup.service -n 100 --no-pager
```

## 遥测通道

| 通道 | Topic | 默认周期 | 用途 |
|---|---|---|---|
| 完整快照 | `{namespace}/{device_id}/telemetry/snapshot/v1` | 1000 ms | 数据库维护完整状态 |
| 实时状态 | `{namespace}/{device_id}/telemetry/realtime/v1` | 100 ms | 查看当前设备的动态数据 |

两个通道均为 QoS 0、`retain=false`，失败帧直接丢弃。快照包含实时通道的核心动态数据。服务端须先支持新 Topic，再更新设备。详细契约见 [高低频遥测发布协议](docs/高低频遥测发布协议.md) 和 [服务端迁移说明](docs/服务端对接-高低频遥测Topic迁移.md)。

## 主控箱运行三态

主控箱在 `{namespace}/{device_id}/runtime/status/v1` 以 QoS 1、`retain=true` 发布 Pi 控制链、F407 业务链和身份核验三态。状态只在内容变化时发布；每次 MQTT 连接或重连后强制重发当前值，不增加周期心跳。F407 连续 10 秒没有有效业务帧只会令业务状态离线，不会关闭 MQTT 控制入口。

Pi 将首次真实核验的 F407 身份保存到 `identity.binding_file`。重启或串口暂时失联时可用缓存身份恢复 MQTT；身份重新出现后必须一致才允许遥测和需向主控板下发的命令，冲突不会自动改绑。异常断线由 runtime Last Will 发布 `offline|unknown|cached`，从未成功持久化绑定时为 `offline|unknown|unbound`。registration 与运行三态相互独立。协议、维护边界和验收命令见 [设备运行三态发布与验收](docs/设备运行三态发布与验收.md)。

阶段一里程碑三的只读 `inspect_pi_link` 已在 Pi 侧实现：Pi 控制在线且身份为 `verified|cached` 时，返回当前控制链、身份、F407 业务链和串口端点事实，不要求 F407 业务在线，也不向 UART6 发实验帧。它与需下发主控板的命令门禁不同；串口端点已打开不能当作 UART6 通信成功。设备侧格式见 [Pi 控制链自检跨端契约](docs/2026-09-22-阶段一Pi控制链自检跨端契约.md)，部署记录与剩余验收边界见 [Pi 控制链自检实现与验证](docs/阶段一Pi控制链自检实现与验证.md)。直达 Pi 的 MQTT 自检已验证，Server/Web 闭环尚未验收。

## 开发与维护

项目使用 C++23/CMake，在树莓派 ARM64 上原生构建。开发机修改后推送，树莓派拉取并执行 `./scripts/deploy.sh`；新增依赖时重新执行 `./scripts/install_deps.sh`。脚本会配置 TUNA apt 源及 GitHub 镜像重写；镜像不可用时按部署手册处理。

- [V1 设计文档](docs/V1设计文档.md)：架构、协议与身份策略。
- [V1.0 树莓派端后续任务清单](docs/CNS_V1.0_树莓派端后续任务清单.md)：对照单机教学闭环梳理必须完成及可延后事项。
- 后续架构目标是由 mavlink-router 独占串口，业务解析与经服务器 5760 入口的远程 MAVLink 透传分路；远程透传前按服务器注册协议握手。该架构尚未实施。
- [协作规则](docs/协作规则.md)：分支、中文提交说明、注释和验证要求。
- [M7 系统化部署设计](docs/M7系统化部署设计.md)：服务、配置、日志和部署边界。
- [设备运行三态发布与验收](docs/设备运行三态发布与验收.md)：运行三态、身份绑定、部署回滚和跨端验收证据。

新确认的设计和选型须同步文档。真实配置与构建产物不提交到仓库。
