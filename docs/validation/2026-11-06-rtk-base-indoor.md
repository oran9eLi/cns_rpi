# RTK 基站第一实验：2026-11-06 室内验证记录

> 历史记录：本文件对应场景纠正前的两串口、PX4 独立注册实现。用户现已明确一个 CNS 箱 F407 身份、三条独立串口，并授权已有合法箱子绑定且无冲突时使用缓存身份上报 RTK。下面数值、摘要和原始 JSON/JSONL 保留，不代表当前拓扑、F407/G2 共存或缓存箱子发布已通过。当前契约见 [操作与接口](../RTK基站第一实验操作与接口.md)，最新缓存箱子身份实测见 [Pi 与云端验证记录](2026-11-06-rtk-box-cached.md)；F407 接回后的实体三链路/G2 同时运行仍未验。

上午阶段完成 Pi 端早期实现和真实 9Ps 基站链路验证。用户当时确认无人机尚未上电，并决定“稍后上电，先收尾基站部分”，因此真实 3DR→PX4 身份→MQTT 联调留待机载端上电后完成。

当日下午用户上电后，旧方案完成真实数传、PX4 身份、独立 PX4 注册的云端 MQTT 双向和快照落库续验，并修复重新开始观测的导航周期时序问题。历史结果见[续验记录](2026-11-06-rtk-cloud.md)，下文保留上午阶段的实际范围。

## 代码与环境

- 分支：`codex/rtk-base-station`；基线为当日获取的 `origin/main`，提交 `d4c1dbc9b8a1049271843d7d2b7e0e18d906691b`。
- Pi：`raspberrypi`，ARM64，Debian GCC 14.2.0，8 GB 内存；Wi-Fi 已切至用户指定网络，本轮 IP 为 `192.168.0.244`。
- 基站：C-RTK 9Ps；实际 MON-VER 为 `ZED-F9P / HPG 1.50 / PROTVER 27.50`，hardware `00190000`。原始回复见 [真实 MON-VER fixture](../../tests/fixtures/rtk/9ps_mon_ver_hpg_1_50.hex)。
- 基站端口：`/dev/serial/by-id/usb-u-blox_AG_-_www.u-blox.com_u-blox_GNSS_receiver-if00`，本轮指向 `ttyACM0`。
- 地面 3DR：`/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0`，本轮指向 `ttyUSB0`。
- 隔离目录：`/home/dcdw/cns-rpi-rtk-base-20261106/`。源码、构建和证据均位于此处，现场认证只留在 Pi 的权限为 0600 的私有配置中。

上传前生成 433 个源码/配置模板/测试文件的 SHA-256 清单，Pi 解包后逐项校验。最终清单文件 SHA-256 为 `7ddd3e2f7e8d18a5ba99e4a0e7a480b1f26a6ef82f647625143cf6bf38a2526f`。测试可执行文件后续补充断言时重新同步并复测，业务源码保持与真机测试一致。

实际 ARM64 `build/cns_rpi` SHA-256：`264ca81405b514aa12dd73235df1419ccdb7ff0fe5e714a14af70cee3c36d57a`。

## 构建与软件链路

| 验证 | 实际结果 |
|---|---|
| 最新 main 基线，WSL Ubuntu-24.04 / GCC 15 | 62/62 CTest 通过 |
| 新代码全量构建，`-Wall -Wextra` | 成功，无编译警告 |
| 新代码全量 CTest | 67/67 通过，60.10 秒 |
| 加强重复动作回执断言后，真实进程＋PTY＋隔离 Broker | 1/1 再次通过，9.65 秒 |
| Pi 原生 ARM64，Release `-O1 -DNDEBUG` | 目标程序及 6 个相关测试可执行文件构建成功 |
| Pi 的 base/session/request/experiment/config/registration | 6/6 CTest 通过；最终同步后复测 0.02 秒 |

Pi 首次全量编译出现 5 条既有 `-Wformat-truncation` 诊断，分别位于未修改的 `pi_link_inspection.cpp:100`、`link_lab.cpp:107`、`deferred_serial_request.cpp:24`、`m1_transaction.cpp:59`、`uart_echo_transaction.cpp:45`，均是原有 UTC 格式化函数。为保持本次 RTK 改动范围，记录保留；不把 ARM64 构建描述为零警告。新增 RTK 源码没有产生警告。

真实进程集成测试使用官方 MAVLink 编码器、两个伪串口和独立本地 Mosquitto，覆盖 PX4 身份、注册、基站遥测、配置双 ACK/读回、重复动作、过期后的迟到 ACK、基站静默和飞控心跳失效。所有模拟身份仅发送到隔离 Broker。

复审发现并修复了三个可复现问题：损坏/残缺 UBX 头堵塞后续解析；新配置期间旧 NAV-SVIN 被误判完成；动作过期后迟到 ACK 继续启动测量。均有先失败、修复后通过的回归证据。

## 真实设备：组合根只读运行

使用独立完整配置和两个已确认的 by-id 路径，以 `dcdw` 用户运行新 `cns_rpi`。暂时停止旧服务释放端口，结束后恢复。启用入口只查询接收机，不自动开始 Survey-in。

[组合根实测状态](2026-11-06-rtk-base/gateway-without-powered-px4.json) 显示：

- 基站端口已打开，接收机核验通过，MON-VER 与 NAV-SVIN 持续更新。
- 真实 UBX 版本、软件/固件/硬件信息可读；校验和成帧错误为零。
- `survey_in.valid=false`、坐标为 null，如实呈现室内尚无有效观测的状态。
- PX4 串口已打开，`identity_verified=false`、`device_id=null`，本地基站观测继续运行。

地面 3DR 在 9600/19200/38400/57600/115200/230400 六档各监听 4 秒并发送 GCS 心跳，均为接收零字节。随后用户确认无人机未上电，此结果不用于诊断数传故障，也不能确定真实工作波特率。

## 真实设备：显式 RAM 配置与数据读取

为在未上电的飞控之外先验证基站，本轮使用临时探针直接链接同一份产品 `cns_rpi_core`，调用实际 `BaseStation::StartSurveyIn(60, 2.0)`；这一步验证基站驱动，MQTT 动作入口另由前述隔离进程测试验证。

流程为读取 MON-VER并核验型号/协议 → 显式设置 RAM 时间模式关闭 → 设置 RAM Survey-in 与 USB 消息 → 收到两次设备 ACK → 读回 13 个配置键并逐项核对 → 继续读取真实 NAV-SVIN/RTCM。

[完整实测 JSONL](2026-11-06-rtk-base/real-base-ram-survey.jsonl) 最后一条结果：

| 字段 | 真实结果 |
|---|---|
| `configuration.state/storage` | `verified / RAM` |
| 测量参数 | 最短 60 秒，平均位置精度门限 2.0 米 |
| `survey_in.active/valid/completed` | `true / false / false` |
| 测量时长 | 2、4、6、8、10、12、14、15 秒递增 |
| `survey_in.observations/ecef_m` | `0 / null` |
| 最终 `i_tow_ms/sample_sequence` | `1678000 / 15` |
| 接收总字节 / 有效 UBX / NMEA | `13123 / 34 / 255` |
| 有效 RTCM | 3 帧、30 字节，均为类型 1230 |
| UBX checksum / framing / RTCM CRC 错误 | `0 / 0 / 0` |

记录 SHA-256：`6dd8580314c3c846d2227bced8e294cad0d3edb9c0ed9ee0093bb0fb470f88cb`。

这里证明了真实基站的配置、ACK、读回、测量状态和 RTCM 解析链路。尚无有效坐标，也未观察到完整的 1005/MSM 改正消息组合。RTCM 1230 的输出周期为 5 个导航周期，因此在本轮只有该类型输出时，默认 3 秒新鲜度门限会在两帧之间显示 stale；计数和年龄保留真实结果。

## 历史结束状态与当时待验项

临时探针和新应用均已退出。原 `cns-rpi.service` 已恢复 active；原 `/var/lib/cns-rpi/config.json` 与 `/home/dcdw/cns_rpi/build/cns_rpi` 的 SHA-256 前后一致。基站本次 Survey-in 设置保留在接收机 RAM；没有保存到 Flash/BBR，重新上电后须按实际读回状态确认。

| 待验项 | 条件与判据 |
|---|---|
| 真实 3DR/PX4 | 无人机飞控和机载数传上电；确认实际波特率，收到 PX4 autopilot 心跳与合法 Basic ID |
| 真实 MQTT | 沿真实 PX4 `device_id` 接收注册、`telemetry.rtk.base`、查询和配置动作 ACK |
| Server/Web | 另行适配设备准入和动作入口，再确认页面显示；本次未修改服务器 |
| 室外 Survey-in | 新鲜 `valid=true`、`active=false`，有效坐标与完整改正流 |
| 第二实验 | RTCM 注入、移动站 Fixed/退化/恢复，另行实现与验收 |
| 供电/网络 | 移动电源、整箱搬运、纯 5G 和长时间稳定性单独验证 |

上午阶段真实上云验收未完成。该早期实现依赖实时 PX4 身份；未核验身份时只有基站本地观测。这一业务身份方案已被场景纠正，不能用于当前产品：当前 MQTT 只用 F407 箱子绑定，PX4 ID 仅为实验元数据；已有合法绑定且无冲突时允许缓存箱子 RTK 帧，无绑定时仍仅本地观测。详见 [第一实验操作与接口](../RTK基站第一实验操作与接口.md)。
