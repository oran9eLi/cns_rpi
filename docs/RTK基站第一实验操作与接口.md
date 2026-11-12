# RTK 基站第一实验操作与接口

服务器可用 [Topic 上报周期控制接口](Topic上报周期控制接口.md) 热改快照/实时周期，基站数据随对应遥测 Topic 更新。调频不重启、不取消测量，不改变接收机输出速率；实验 ACK 保持立即发送。

> 2026-11-09：第一实验操作与身份合同保持。移动站改正转发、暂停/恢复和真实协议教学样本是后续显式增量，见 [移动站实验与教学数据接口](RTK移动站实验与教学数据接口.md)；下文“不转发”描述原第一实验边界，不再代表新增移动站操作。

版本：2026-11-06  
范围：CNS 箱内 Pi 的 F407、3DR/PX4、9Ps 三链路组合，基站识别、Survey-in RAM 配置、实时观测与箱子 MQTT 接口。

## 1. 实验目标与边界

第一实验让学生观察基站接收机身份、测量进度、平均 ECEF 坐标与精度，以及经过校验的 RTCM 输出。配置读回完成后，继续用真实 `NAV-SVIN` 判定测量是否完成。当前实现不把 RTCM 转发到飞控、不建立 NTRIP 服务，也不以飞控 GPS 状态判定基站测量成功。

`rtk_base.enabled` 默认 `false`，缺少该配置的旧文件仍走原 F407 流程。显式启用后在同一个主循环增加 RTK 模块，F407 常驻，G2、`config/set`、`control/set` 和运行三态继续工作。MQTT 只使用箱子 F407 的业务身份，PX4 Basic ID 仅作实验元数据。

现场本轮识别的接收机为 ZED-F9P，固件 HPG 1.50、协议 27.50；固件与协议版本对应关系见 [u-blox 官方 HPG 1.50 接口规范](https://content.u-blox.com/sites/default/files/documents/u-blox-F9-HPG-1.50_InterfaceDescription_UBXDOC-963802114-12815.pdf)。源码识别边界为 ZED-F9P、协议 27.12/27.31/27.50；其他型号或协议明确报告未支持。型号识别通过只说明当前接收机及版本满足本实现边界，不能替代 Survey-in 或室外验收。研发 Pi 本轮地址为 `192.168.0.244`，地址可能变化，不属于程序常量。

## 2. 接线、端口与身份

| 项目 | 连接与判断 |
|---|---|
| F407 | 正常整箱拓扑连接主控 F407；`serial.device/baud` 只属于该链路；本轮允许暂缺 F407 并沿合法缓存箱子绑定验证 RTK |
| PX4 | 飞控上电，经明确的串口送入 MAVLink；以 PX4 autopilot 组件心跳选定端点 |
| 基站 | 基站接收机独立 USB 口直连 Pi，接收 UBX/NMEA/RTCM 原始字节 |
| 端口所有者 | 三个不同设备分别独占；同一进程内各只有一个读写者 |
| MQTT 业务身份 | 沿用 F407 的合法 Basic ID、持久绑定及 `cns_box` 类型；不为 PX4、基站或 Pi 另建业务身份 |
| 本地观测 | 没有 PX4 实时身份也继续运行，写入本地状态文件；无箱子绑定时只保留本地 base/PX4 |

研发现场曾对应为 PX4 链路 `/dev/ttyUSB0`、基站 `/dev/ttyACM0`，仅作为当次枚举结果。正式配置优先使用现场确认的 `/dev/serial/by-id/...` 或 `/dev/serial/by-path/...`，并检查链接实际指向；不能用该历史编号认领设备。`rtk_base.px4_device` 与 `rtk_base.device` 须为不同的明确绝对路径，并与 F407 端口分开；相同设备的路径别名也不能重复占用。F407 的 `serial.device` 仍可为 `auto`，自动发现排除 RTK 两个设备及其别名。不要同时运行会占用相同端口的串口工具或另一业务进程。

Pi 在 PX4 独立链路每秒发送一次 GCS 心跳；发现 PX4 后，在未核验身份期间每两秒请求一次 `OPEN_DRONE_ID_BASIC_ID`（消息 12900，见 [MAVLink 官方字段定义](https://mavlink.io/en/messages/common.html#OPEN_DRONE_ID_BASIC_ID)）。飞控没有返回该消息时继续等待，不从飞控 UID、GUID 或 Pi CPU 序列号补身份。PX4 标准遥测须经过该会话实时核验，才放入 `telemetry.rtk.px4`；不得覆盖 F407 的共享状态或业务身份。

PX4 心跳静默 5 秒、串口断开或 Basic ID 冲突时，只清空该实验会话的实时身份和遥测；重连须重新取得同一 Basic ID。进程内首个 PX4 身份和端点保持约束，其他 system 或冲突身份不能自动换机。箱子 MQTT、F407 绑定/运行三态及 G2 不因 PX4 失联而关闭。用户授权 F407 暂未连接时，已有合法持久化 `cns_box` 绑定且无身份冲突即可发布 RTK；帧内不带旧 F407 普通传感数据，运行三态如实报告 cached/offline/unknown。当前 F407 身份是否已核验由 `telemetry.rtk.box_identity_verified` 表达，不将缓存身份冒充实时业务在线。

## 3. 配置与研发启动

将下面两段合入现有完整现场配置，保留现场 MQTT 地址、认证及其他配置；这不是可单独启动的完整 JSON：

```json
{
  "serial": {
    "device": "auto",
    "baud": 115200
  },
  "rtk_base": {
    "enabled": true,
    "device": "/dev/serial/by-id/REPLACE_WITH_BASE_USB",
    "baud": 115200,
    "px4_device": "/dev/serial/by-id/REPLACE_WITH_PX4_LINK",
    "px4_baud": 57600,
    "survey_min_duration_s": 60,
    "survey_accuracy_m": 2.0,
    "stale_after_ms": 3000,
    "status_file": "/tmp/cns-rpi-rtk-test/status.json"
  }
}
```

| 配置 | 范围与含义 |
|---|---|
| `enabled` | 默认 false；启用时增加 PX4 和基站两条链路，保留 F407 主流程 |
| `serial.device/baud` | F407 端口与波特率；可保持 `auto`，发现排除 RTK 设备；不用于 PX4 |
| `rtk_base.device/baud` | 9Ps 基站明确绝对路径与波特率，默认 115200 |
| `rtk_base.px4_device/px4_baud` | 3DR/PX4 明确绝对路径与波特率，默认 57600；与另外两条链路不同 |
| 各串口波特率 | 分别配置，支持 9600/19200/38400/57600/115200/230400；按现场设备实测选择 |
| `survey_min_duration_s` | 10..86400 秒；本地默认 Survey-in 参数，加载不会自动开始测量 |
| `survey_accuracy_m` | 0.1..100 米；默认平均位置精度门限 |
| `stale_after_ms` | 1000..60000 毫秒；基站样本新鲜度门限 |
| `status_file` | 非空绝对路径；默认 `/run/cns-rpi/rtk-base-status.json`，研发示例改用当前用户可写目录；约每秒以临时文件加 rename 更新 |

接收机版本与测量状态的查询间隔为 `min(1000ms, stale_after_ms / 2)`。默认 `3000ms` 门限仍每秒查询一次；合法下限 `1000ms` 则每 `500ms` 查询一次（2Hz），避免配置过程中版本证据过期。此前固定 1Hz 的 MON-VER 查询与 `1000ms` 门限竞争，在缓存箱子身份、F407 未连接且主循环约每 `100ms` 运行一次时可导致配置失败；本轮缩短查询间隔，未放宽新鲜度判定。

研发联调用隔离目录和完整配置，不在同一组串口上并行启动生产服务与测试进程。按授权释放端口、备份并保留原配置后，可前台运行已构建程序；测试配置仍须保留真实 F407 绑定路径与箱子身份，不得把 PX4 ID 写入该绑定：

```bash
./build/cns_rpi /绝对路径/rtk-test-config.json --check-config
./build/cns_rpi /绝对路径/rtk-test-config.json --config-writer=disabled
python3 -m json.tool /tmp/cns-rpi-rtk-test/status.json
```

启动仅查询版本和测量状态，不自动改接收机参数。收到合法 Survey-in 动作后，执行“RAM 关闭原时间模式 → 等待新导航周期确认观测已停止 → RAM 设置 Survey-in/USB 输出 → RAM 读回核对”。接收关闭配置的 ACK 与实际停止观测存在导航周期延迟，停止确认超时会报告失败。配置不保存到 BBR/Flash，不修改接收机 UART1 或升级固件。USB 输出启用 UBX、RTCM 与观测消息，保留现场 NMEA 设置；重新上电后的参数以接收机实际状态为准，配置文件本身不会自动重放测量动作。

### 3.1 研发正式常驻部署

先备份现场配置，仅将上例的 `rtk_base` 段合入原 `/var/lib/cns-rpi/config.json`，用已确认的两个实际端口替换占位符，并将 `status_file` 设为 `/run/cns-rpi/rtk-base-status.json`。保留原 `serial`、MQTT、`identity.binding_file` 及其他非 RTK 配置和原箱子绑定文件，不用示例整文件覆盖，也不把 PX4 ID 写入绑定。

`scripts/deploy.sh` 保留已有现场配置；修改仓库样例或 `config/config.json` 不会自动启用已部署设备的 RTK。以 `dcdw` 用户在 `/home/dcdw/cns_rpi` 执行下列命令，先校验实际现场配置，再构建并安装、启用和启动或重启常驻服务；不要用 `sudo ./scripts/deploy.sh` 启动部署脚本：

```bash
cd /home/dcdw/cns_rpi
./build/cns_rpi --check-config /var/lib/cns-rpi/config.json
./scripts/deploy.sh
systemctl is-enabled cns-rpi.service
systemctl status cns-rpi.service --no-pager
journalctl -u cns-rpi.service -n 100 --no-pager
python3 -m json.tool /run/cns-rpi/rtk-base-status.json
```

查看状态时同时检查服务存活与文件更新时间。常驻服务启动、重启或开机自启只恢复链路与状态观测，不自动重放 Survey-in；需要开始新一轮测量时，仍须显式发送后文的 `rtk_base_survey_in` 动作。

## 4. 平台步骤与判据

Pi 端已提供下列动作和事实，直达 MQTT 操作及现有 HTTP 快照已实机验证；Server 实验 Session/动作入口与网页操作页尚未接入。学生界面可按表组织，下面命令仅供研发联调。

| 步骤 | 调用或观察 | 可确认的结果 |
|---|---|---|
| 连接设备 | 箱子运行三态、本地 `px4`、`base` 状态 | F407 身份/业务在线；分别报告 PX4 串口与身份核验、基站型号/协议核验 |
| 查询基站 | `rtk_base_status` | 返回本次采样事实，未知与过期单独显示 |
| 开始测量 | `rtk_base_survey_in`，显式给定时长和精度 | `accepted` 仅表示配置动作被接受 |
| 核对配置 | 同动作最终 ACK、`configuration.state` | `completed`/`verified` 表示 RAM 参数读回匹配 |
| 观察测量 | 查询或 `telemetry.rtk.base.survey_in` | 新鲜 `valid=true`、`active=false` 且接收机/配置有效时，`completed=true` |
| 观察输出 | `rtcm` 计数和新鲜度 | 只统计 CRC24Q 通过的帧、字节和类型；未转发到移动站 |

室内可能持续无有效坐标、测量不完成或 RTCM 输出为零，应原样展示。Survey-in 的 valid 与移动站 RTK Fixed 是不同事实，本实验不输出移动站已 Fixed 的结论。

## 5. MQTT 主题与注册

| 用途 | Topic | QoS / retain |
|---|---|---|
| 注册 | `{namespace}/{device_id}/registration` | 配置值，默认 2 / true |
| 快照 | `{namespace}/{device_id}/telemetry/snapshot/v1` | 0 / false，默认 1000 ms |
| 实时 | `{namespace}/{device_id}/telemetry/realtime/v1` | 0 / false，默认 100 ms |
| RTK 下发入口 | `{namespace}/{device_id}/experiment/set` | 2 |
| 实验回执 | `{namespace}/{device_id}/experiment/ack` | 2 / false |

表中 `device_id` 始终是箱子 F407 的 Basic ID。沿用箱子 schema 3 注册、`device_type=cns_box`、产品与节点标签、既有能力和运行三态；启用 RTK 时追加 `rtk_base` 能力，不创建独立 `flight_controller` 注册。`config/set`、`control/set` 和 G2 仍使用原有箱子入口。快照 schema 为 3，实时帧 schema 为 1，原第一实验在两类帧增加 `telemetry.rtk={base,px4,box_identity_verified}`。2026-11-09 移动站增量追加 `relay/observation` 和定位观察，低频保留协议原文与历史，高频省略这些样本，详见[新接口4.1](RTK移动站实验与教学数据接口.md#41-原文采样和带宽)；`px4` 中的身份属于实验元数据，布尔值 `box_identity_verified` 表示当前 F407 链路身份是否已核验。

MQTT 建立、重连、遗嘱及正常退出沿用 F407 `DeviceSession` 与持久绑定机制。PX4 实时身份失效只改变 `px4` 事实，基站失联只改变 `base` 事实，均不改变箱子身份或关闭箱子 MQTT。已有合法持久化箱子绑定且无身份冲突时，F407 未连接也可发布缓存身份 RTK 帧；F407 未实时业务在线时只携带 RTK 实验事实，不夹带旧 F407 普通传感数据。无绑定时仅本地观测，不将 PX4 ID 用作备用 MQTT 身份。

缓存 RTK 快照仍须满足服务器现有 `ParseTelemetry` 的 schema 3 结构：根级 `drone_id` 和其 `basic_id` 必须为对象。因此该快照保留精确空容器 `"drone_id":{"basic_id":{}}`，其中不含 `id_type`、`ua_type` 或任何 F407 实时观测值；空容器不代表 F407 身份已实时核验。实时帧 schema 1 仍不带 `drone_id`。此兼容处理只在 Pi 侧补结构，服务器代码未改。

## 6. 下发请求

两个操作共用严格 schema 1。所有关联 UUID 按实际命令生成；`request_id` 必须等于 `action_id`。`target.device_id` 与 Topic 必须指向同一箱子 F407 业务身份，不能使用 PX4 Basic ID。`lease_version` 为正整数，同一最高租约不能被另一 session 复用；更低租约拒绝。

只读查询没有 `parameters` 字段：

```json
{
  "schema_version": 1,
  "command_id": "00000000-0000-4000-8000-000000000001",
  "request_id": "00000000-0000-4000-8000-000000000003",
  "target": {"device_id": "CNS_BOX_DEVICE_ID"},
  "session_id": "00000000-0000-4000-8000-000000000002",
  "action_id": "00000000-0000-4000-8000-000000000003",
  "lease_version": 1,
  "operation": "rtk_base_status",
  "expires_at": "2026-11-06T12:00:30.000Z"
}
```

示例用于说明字段，实际发送前替换身份、UUID 和未来的 UTC 毫秒过期时间。Survey-in 使用新的 action/request ID，并增加两个显式参数：

```json
{
  "schema_version": 1,
  "command_id": "00000000-0000-4000-8000-000000000005",
  "request_id": "00000000-0000-4000-8000-000000000004",
  "target": {"device_id": "CNS_BOX_DEVICE_ID"},
  "session_id": "00000000-0000-4000-8000-000000000002",
  "action_id": "00000000-0000-4000-8000-000000000004",
  "lease_version": 1,
  "operation": "rtk_base_survey_in",
  "expires_at": "2026-11-06T12:00:30.000Z",
  "parameters": {
    "minimum_duration_s": 60,
    "accuracy_limit_m": 2.0
  }
}
```

`minimum_duration_s` 为整数 10..86400，`accuracy_limit_m` 为有限数值 0.1..100，单位分别为秒和米。两项均必填，动作不改现场配置中的默认值。额外字段、错误类型、未知操作或超过 4096 字节的载荷均拒绝解析。不能安全关联的非法请求不生成猜测性 ACK。

研发可先订阅 ACK 与遥测，再向当前身份的固定 Topic 发布完整请求文件：

```bash
mosquitto_sub -h BROKER -p 1883 -q 2 -t 'NAMESPACE/CNS_BOX_DEVICE_ID/experiment/ack' -v
mosquitto_sub -h BROKER -p 1883 -t 'NAMESPACE/CNS_BOX_DEVICE_ID/telemetry/snapshot/v1' -v
mosquitto_pub -h BROKER -p 1883 -q 2 -t 'NAMESPACE/CNS_BOX_DEVICE_ID/experiment/set' -f request.json
```

## 7. ACK、重投与完成语义

ACK 固定包含下列字段，`fact.base` 是完整基站状态；这里用空对象只展示结构：

```json
{
  "schema_version": 1,
  "command_id": "00000000-0000-4000-8000-000000000001",
  "session_id": "00000000-0000-4000-8000-000000000002",
  "action_id": "00000000-0000-4000-8000-000000000003",
  "operation": "rtk_base_survey_in",
  "observed_at": "2026-11-06T12:00:00.000Z",
  "status": "accepted",
  "fact": {"base": {}}
}
```

状态查询返回 `completed`，只表示完成这次状态采样。测量动作先返回 `accepted`，收到配置 ACK 且 RAM 读回一致后返回 `completed`；拒绝为 `rejected`，已接受后的配置失败为 `failed`。测量动作的最终 `completed` 不表示 Survey-in 已完成，后者持续通过 `base.survey_in.completed` 观察。

失败时增加 `error_code`，包括 `wrong_target`、`action_conflict`、`expired`、`stale_lease`、`action_cache_full`、`configuration_busy`、`base_configuration_rejected`、`base_configuration_failed`、`base_disconnected`、`configuration_timeout`。配置超时只说明动作在期限内未取得完成证据，接收机后续事实仍以状态查询为准。

同 `action_id`、同原文语义的重投复用首次结果，仅允许更换合法 `command_id`；不要更改参数、过期时间或租约后沿用 action ID。要刷新只读状态，生成新 action ID。进程内最多保留 128 项动作，终态保留至少 300 秒，重启不保留此去重缓存。实验 ACK 原文保留在有界队列，收到 MQTT QoS 完成回调后才删除；入队成功不代表 ACK 已交付。

## 8. 本地状态与基站字段

`rtk_base.status_file` 不包含 MQTT 密码。`box` 属于 F407 箱子，`px4.device_id` 仅为实验元数据；`box.identity_verified` 与遥测的 `rtk.box_identity_verified` 表达同一当前核验事实。下面是没有箱子绑定、设备未接入时的结构摘录，实际 `base` 还有后表所列字段：

```json
{
  "schema_version": 1,
  "source": "live",
  "box": {
    "device_id": null,
    "identity_verified": false
  },
  "px4": {
    "serial_connected": false,
    "identity_verified": false,
    "device_id": null,
    "frame_age_ms": null
  },
  "base": {
    "connected": false,
    "receiver_verified": false,
    "configuration": {"state": "idle", "storage": "RAM"},
    "survey_in": {"known": false, "active": null, "valid": null, "completed": false}
  }
}
```

| 基站字段 | 事实含义 |
|---|---|
| `connected/device/baud` | 当前串口持有状态与已配置端点；不是通信成功结论 |
| `receiver_verified` | 新鲜 MON-VER 确认 ZED-F9P 和受支持协议 |
| `rx_bytes/last_rx_age_ms` | 当前连接的接收字节数与最近任意字节年龄 |
| `protocol` | UBX 版本、software/firmware/hardware/module、版本样本年龄、stale、有效帧/校验/成帧错误计数 |
| `survey_in.known/active/valid/completed/stale` | 真实 NAV-SVIN 是否已知、是否测量、有效性、组合完成判据及样本过期情况 |
| `survey_in.ecef_m` | 平均 ECEF `x/y/z`，单位米；未知为 null |
| `duration_s/mean_accuracy_m/observations/i_tow_ms` | NAV-SVIN 测量时长、平均精度、观测数和 GNSS 时间 |
| `sample_sequence/last_rx_age_ms/last_sample_age_ms` | 收帧与新观测分开；重复相同 iTOW 不刷新样本时间 |
| `rtcm` | 通过 CRC24Q 的 valid_frames、bytes、message_types，crc_errors 和年龄/stale |
| `nmea` | 校验通过语句数和校验错误数，不用于宣称定位完成 |
| `configuration` | state、storage=RAM、本次请求时长/精度、verified_age_ms |
| `last_error` | 当前接收机不支持、串口或配置错误；没有时为 null |

`configuration.state` 为 `idle/pending/awaiting_ack/awaiting_stop/awaiting_readback/verified/failed`。`awaiting_stop` 表示关闭时间模式已获 ACK，正在等待下一导航周期的 inactive 确认，最长 3 秒。`survey_in.completed` 要求接收机新鲜有效、当前配置状态可用、新鲜 NAV-SVIN、`valid=true` 且 `active=false`；只收到重复旧观测、配置 ACK 或 RTCM 均不会令其完成。断开重连清除旧连接的观测、版本、配置核验和计数证据。

本地文件约每秒刷新；读取者还须检查文件更新时间和进程存活，不能把进程停止后留下的最后一份文件当成正在观测。`source=live` 表示内容来自本次进程的现场观测，不能据此推断云端已收到。

## 9. 验收记录

最新结果见 [树莓派常驻交付](validation/2026-11-06-rtk-base-service.md)：已通过正式 `scripts/deploy.sh` 部署并保留新主服务 active/enabled，原箱子绑定和非 RTK 配置不变。正式 canonical 构建的原生相关测试 8/8；43 帧 RTK 遥测/50 条消息、查询和 Survey-in RAM 配置回执通过，HTTP 快照实际推进并含配置 `verified`。显式重启后两条链路恢复，查询再次完成，30 条 1Hz 状态正常、`NRestarts=0`，启动未自动重放测量动作。室内测量仍无有效坐标。

此前 [缓存箱子身份的隔离验证](validation/2026-11-06-rtk-box-cached.md)记录 WSL 全量 68/68 CTest（19.82 秒）、Pi 相关 8/8，以及替换正式程序前的实测和原服务恢复；其中“未替换正式程序”仅适用于该隔离轮次。本次常驻交付后未提交或推送；F407 接回后的实体三链路/G2 同时运行、室外 Survey-in 成功、移动站 Fixed 和网页实验页面仍未验或未做，不能称为整体验收通过。

早期真实基站配置与观测证据、构建结果以及未上电飞控的条件见 [2026-11-06 室内验证记录](validation/2026-11-06-rtk-base-indoor.md)。该记录属于场景纠正前的实现，基站驱动原始证据保留。

当日下午旧独立 PX4 注册实现的真实 3DR / PX4 / MQTT / 正式 API 快照落库及观测重启修复见[历史云端续验记录](validation/2026-11-06-rtk-cloud.md)。原始 JSON/JSONL 不改；其中 PX4 ID 注册和两串口进程证据不能代表当前箱子身份、三独立串口、F407/G2 同时运行通过。历史现场确认 3DR 为 57600、基站 USB 为 115200；当前仍须分别配置并复核设备。
