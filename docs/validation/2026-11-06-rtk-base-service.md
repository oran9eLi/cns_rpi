# 2026-11-06 RTK 第一实验：树莓派常驻交付

本次把已完成隔离联调的第一实验 Pi 实现部署到正式 `cns-rpi.service`，退出后保留新服务运行。交付范围为基站与 PX4 两条附属链路、唯一 CNS 箱身份、MQTT 查询/开始测量及现有服务器 HTTP 快照。未修改服务器或 F407，未提交或推送。

## 部署与保留项

- 分支 `codex/rtk-base-station`，基于 main `d4c1dbc9b8a1049271843d7d2b7e0e18d906691b`。
- 实机 `dcdw@192.168.0.244`，原 canonical 工作树为干净的 v1.0.5。同步 465 个源码、测试、部署脚本和 systemd 文件，逐文件 SHA256 核对一致。
- 由 `dcdw` 在 `/home/dcdw/cns_rpi` 执行项目唯一支持的 `scripts/deploy.sh`，原生 Release 构建和部署返回 0，版本报告 `cns_rpi 1.0.7`。脚本安装 helper、启用并重启主服务与通信服务。
- 正式程序 `/home/dcdw/cns_rpi/build/cns_rpi` 的 SHA256 为 `ca9abd849b4c3d012789c215199c598aa8b8ca5d9a9cdf96bbdd73847a22c24c`，已核对运行进程 `/proc/PID/exe` 与该文件一致。此处为 canonical Release 构建，不沿用先前隔离构建的摘要。
- 正式配置 `/var/lib/cns-rpi/config.json` 仅增加 `rtk_base`。原串口、MQTT、身份、网络及其他非 RTK 配置语义不变，原箱子绑定文件 SHA256 不变。真实认证配置没有进入仓库证据。
- 备份保存在实机 `/home/dcdw/cns-rpi-rtk-base-20261106/deploy-first-20261106T064744Z`，目录为 root 0700，包含原源码、程序、配置、绑定及服务文件。本次成功后未执行回滚。

正式 RTK 配置使用 9Ps 的 USB by-id 路径、115200，以及 CP2102 地面 3DR 的 USB by-id 路径、57600；两者独立于 F407 `serial=auto`。默认测量参数 60 秒/2 米，新鲜度 3000ms，状态文件 `/run/cns-rpi/rtk-base-status.json`。具体非敏感配置与部署核对见[部署检查](2026-11-06-rtk-base-service/deployment-inspection.json)，[部署后半段输出](2026-11-06-rtk-base-service-deploy-tail.log)保留约构建 50% 起的日志。

## 正式服务真实验证

验证时 F407 尚未连接，按用户授权沿原绑定 `DCDWCNS1GHC0G6LF8MY6` 注册 `cns_box`。基站为真实 ZED-F9P / HPG 1.50 / PROTVER 27.50；PX4 子 ID 为 `393233510c004b0031`。本次经 Wi-Fi 接入服务器，不代表 5G 验收。

| 检查 | 实际结果 | 证据 |
| --- | --- | --- |
| 正式 canonical 构建的相关原生 CTest | 8/8 通过，1.01 秒 | [测试日志](2026-11-06-rtk-base-service/native-tests.log) |
| 唯一云端身份 | 原箱子注册含 `rtk_base`，runtime 身份为 cached，`box_identity_verified=false` | [MQTT 汇总](2026-11-06-rtk-base-service/mqtt/summary.json) |
| 基站、数传和 PX4 | 43 帧 RTK 遥测中均有真实 PX4 子观测，共记录 50 条目标消息 | [消息原文](2026-11-06-rtk-base-service/mqtt/messages.jsonl) |
| `rtk_base_status` | 返回 `completed` 和当前基站事实 | [请求原文](2026-11-06-rtk-base-service/mqtt/requests.jsonl)及上述汇总 |
| `rtk_base_survey_in` | `accepted` → `completed`；RAM 参数读回 `verified`、60 秒/2 米 | 同上 |
| 缓存身份真实性 | 没有旧 F407 传感值、模块、告警或日志；schema 3 仅保留精确空 `drone_id.basic_id` 对象 | 上述 MQTT 原文及 HTTP 摘录 |
| PX4 注册隔离 | 没有新的独立 PX4 在线注册；1 条历史 retained 注册单独计数 | 上述 MQTT 汇总 |
| 服务器 HTTP 快照 | HTTP 200，`telemetry_received_at=2026-11-06T06:57:11.885Z`，包含配置 `verified` 与 PX4 子 ID | [服务器摘录](2026-11-06-rtk-base-service/server-rtk-extract.json) |

MQTT 操作窗口为 `2026-11-06T06:57:08.456Z–06:57:12.519Z`（北京时间 14:57）。配置 `completed` 表示 ACK 和 RAM 参数读回完成；真实 NAV-SVIN 为 `active=true`、`valid=false`、`completed=false`，没有有效 ECEF 坐标。

## 重启与留运行

`06:57:19.253Z` 显式重启正式主服务，PID 从 `17046` 变为 `17232`。两条 RTK 链路和原箱子身份恢复，重启后的真实状态查询再次 `completed`。随后 30 条 1Hz 本地样本中接收机及 PX4 身份均有效、PID 不变、`NRestarts=0`；这只是短窗口检查，不是长时间稳定性验收。

重启后配置动作状态为 `idle`，没有自动重放 Survey-in。接收机 RAM 中已有观测继续，HTTP 快照在 `06:57:52.803Z` 记录测量时长 42 秒，仍为 active/invalid、没有有效坐标。进程重启后不沿用旧的配置核验结论；需发起新的配置动作才重新获得 `verified`。

最终服务为 `active/running` 且开机自启已启用，新版本保留运行。见[重启汇总](2026-11-06-rtk-base-service/restart-summary.json)、[重启后的查询](2026-11-06-rtk-base-service/restart-query.json)与[30 条状态样本](2026-11-06-rtk-base-service/stability.jsonl)。本次验证的是服务重启，没有给 Pi 或接收机断电。

## 尚待完成

本次交付第一实验的 Pi 常驻服务及 MQTT/现有 HTTP 数据链路。网页按钮、服务器实验 Session/动作入口尚未接入；F407 接回后的实体三链路/G2 并行、室外 Survey-in 成功、长时间与断网恢复仍待验。第一实验当前仅统计经过校验的 RTCM，不转发改正数据；第二实验移动站 Fixed 尚未开始。
