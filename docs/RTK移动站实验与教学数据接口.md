# RTK 移动站实验与教学数据接口

日期：2026-11-09。范围：树莓派端候选实现及服务器接入合同。服务器和网页本轮未修改，页面接入后才能让学生直接使用以下能力。基站操作仍见 [第一实验接口](RTK基站第一实验操作与接口.md)。本文件中的协议说明不代表室外固定解验收。

## 1. 学生可以观察什么

| 链路 | 真实内容 | 可以得出的结论 |
|---|---|---|
| 9Ps → Pi | UBX NAV-SVIN 完整原帧、Survey-in 时间/观测数/平均位置精度/ECEF、RTCM 类型/长度/CRC/十六进制 | 基站是否完成测量、是否正在产生改正数据 |
| Pi → 地面3DR | 实际写入成功的 MAVLink 原帧、#233、flags、sequence/fragment、len、改正有效载荷 | Pi 向数传串口写了什么；不能据此认定机载接收机已使用 |
| PX4 → Pi | GPS_RAW_INT 原帧、源时间、样本序号/年龄、卫星数、fix_type、经纬高和估计精度、定位状态变化记录 | 移动站实际报告的状态及新鲜度 |
| Pi → 云端 | 沿用箱子身份的快照/实时遥测、动作回执、本地转发等待/暂停/运行状态与计数 | 设备到服务器的观测通道是否通畅；不证明网页已正确显示 |

不要求 QGC。改正数据在本地 USB 基站 → Pi → 3DR → PX4 → 移动站路径传输；云服务器负责下发实验操作、展示和记录。摄像头是独立实验。

## 2. 实验步骤

1. 保持 CNS 箱正常身份配置，接入 9Ps USB 和地面3DR，无人机/PX4 上电。合法缓存的 F407 箱子身份可使用，`box_identity_verified=false` 需如实展示。
2. 第一实验发 `rtk_base_survey_in`，等配置读回 `verified`。配置读回完成只表示参数生效；随后观察 `base.survey_in.completed`，不能把它当移动站 Fixed。
3. 发 `rtk_rover_start`。可以在基站未完成时开始，状态为 `waiting_base`；PX4 失联为 `waiting_px4`；基站完成但没有新改正为 `waiting_rtcm`。条件满足后自动传当前改正。
4. 对照完整 RTCM 原帧与其 MAVLink 分片，观察移动站卫星数、`3d/dgps/rtk_float/rtk_fixed`、估计精度和位置变化。
5. 发 `rtk_rover_pause`，只暂停改正，遥测/心跳继续。记录定位是否退化及耗时；不要预设一定立即退化。
6. 发 `rtk_rover_resume`，只传恢复后新到的改正，记录恢复过程。结束发 `rtk_rover_stop`。

进程重启后转发默认为 `stopped`，不会自动重启 Survey-in 或转发。已启动的本地链路不依赖 MQTT 持续在线。基站重新测量会将正在运行的改正转发置为 `paused`，须重新恢复。箱子身份冲突会停止转发。开始、恢复及基站/PX4重新就绪时，现有串口读取者先排除边界前的改正和跨边界半帧，等待随后新帧；UBX配置回执和其他观测继续处理。`base.rtcm.activation_barrier=true` 表示这个有界清理尚未完成。

室内可验证接收机/数传/PX4身份、命令与上云；没有卫星时不以 Fixed 作为通过条件。室外静置无人机也能观察收敛，不需要起飞。

## 3. MQTT 操作

下发：`{namespace}/{箱子device_id}/experiment/set`；回执：相同前缀的 `experiment/ack`。沿用原 QoS、ACK 待发队列及身份门禁。不要用 PX4 ID 作为 target。

五项操作均使用以下精确九字段信封，不带 `parameters`。UUID 为示例，每个新动作使用新 action_id/request_id；重投同一动作只换 command_id。同一次实验的租约由平台管理，不用本机时钟生成极大的租约版本。

```json
{
  "schema_version": 1,
  "command_id": "3f886532-0000-4000-8000-000000000001",
  "request_id": "3f886532-0000-4000-8000-000000000002",
  "target": {"device_id": "DCDWCNS1RTKTEST00001"},
  "session_id": "3f886532-0000-4000-8000-000000000003",
  "action_id": "3f886532-0000-4000-8000-000000000002",
  "lease_version": 1,
  "operation": "rtk_rover_start",
  "expires_at": "2026-11-09T08:00:30.000Z"
}
```

| operation | 行为 |
|---|---|
| `rtk_rover_start` | 从停止开始新一轮，本机 run_sequence 加一；已开始时拒绝，先停止 |
| `rtk_rover_pause` | 暂停转发并清空待发旧改正；未开始时拒绝 |
| `rtk_rover_resume` | 从暂停恢复；不是暂停状态时拒绝 |
| `rtk_rover_stop` | 停止并清空待发；重复停止可以成功 |
| `rtk_rover_status` | 只读完整当前状态，不改变设备 |

`completed` 仅表示这项本地操作已完成。当前事实在 `fact.rtk`，形状与 `telemetry.rtk` 一致；原基站操作保持 `fact.base`，Survey-in 配置仍为 accepted→completed/failed。相同 action_id 的重投返回缓存回执；有效授权请求因当前操作状态被拒绝时也缓存终态，后来重投不会在状态变化后突然执行。要查询最新状态应发新动作。

原目标不匹配、过期、旧租约、动作内容冲突、缓存满错误保持。新增 `rtk_rover_unavailable` 表示未接入移动站执行入口；`rtk_rover_rejected` 表示本次状态转换拒绝或 RTK 未启用。动作输入不接受任意串口数据/任意 MAVLink 指令。

## 4. 遥测合同

现有 `telemetry/snapshot/v1`（schema3）与 `telemetry/realtime/v1`（schema1）及发布周期不变。单一箱子业务身份不变，缓存快照仍含精确空 `drone_id.basic_id={}`。`telemetry.rtk` 新增 `relay`、`observation` 和 `px4.gps_observation` / `px4.firmware`。

启用RTK时注册能力追加 `rtk_base`、`rtk_rover`、`rtk_protocol_observation`。这些能力表示实现已提供入口，不表示设备当前在线、已完成测量或已经Fixed。

### 4.1 原文采样和带宽

完整原文只进低频快照、动作回执、本机 `/run/cns-rpi/rtk-base-status.json`，高频实时遥测省略 `base.survey_in.raw_hex`、`px4.gps_observation.raw_hex/transitions`、`observation.latest_rtcm/latest_transfer`。两类帧都有当前状态、计数、新鲜度。平台应以低频快照更新原文窗口，高频帧缺少原文表示本帧省略，不是抓包记录被清空。

`observation.capture_mode="bounded_latest"`，`complete_capture=false`。只保留最近一个 RTCM 和最近一次转发尝试，容量各1；不是全量抓包，也不是所有消息类型各保留1条。`rtcm_samples_replaced` 是被后续样本替换的数量。服务生命周期计数重启归零；基站连接计数断连后重置，RTCM frame_sequence 在本进程内单调增加。

### 4.2 基站原帧

`base.survey_in.raw_hex` 是校验通过的实际 NAV-SVIN 完整 UBX 帧，未收到时为空；新鲜度沿用 `last_rx_age_ms/last_sample_age_ms/stale`。其结构为 `b5 62 01 3b 28 00 [40字节负载] CK_A CK_B`。

负载关键偏移：iTOW 4..7（ms），dur 8..11（s），meanX/Y/Z 12..23（cm），meanXHP/YHP/ZHP 24..26（0.1mm有符号修正），meanAcc 28..31（0.1mm），obs 32..35，valid 36，active 37。多字节数按小端解释；页面可以对照既有解码字段，无须在浏览器重新实现驱动。

`observation.latest_rtcm`：

| 字段 | 含义 |
|---|---|
| `frame_sequence` | 本进程完整合法 RTCM 帧序号，供转发关联 |
| `message_type` / `purpose` | 前12负载位消息类型和已知用途；未知保留数字与 unclassified |
| `length_bytes` / `raw_hex` | 完整帧长度及原字节，包含D3头和CRC24Q，未截断 |
| `crc_valid` / `age_ms` | 已通过校验、距实际接收的单调时间年龄 |

RTCM3结构：D3前导，随后保留6位及10位负载长度，负载，末3字节CRC24Q。负载首12位为消息号；这里只解出帧封装和消息号，未实现完整观测量解算。1005是参考站坐标、1077/1087/1097/1127分别是GPS/GLONASS/Galileo/北斗MSM7、1230为GLONASS码相位偏差；不能把一条MSM消息称为无人机的位置。

### 4.3 实际转发样本

`observation.latest_transfer` 含 `run_sequence`、`rtcm`（同上，保留当时完整源帧）、`fragments[]`、`complete`、`age_ms`、`evidence="local_serial_write"`。其 `rtcm.frame_sequence` 对应当次源帧，可能与后来收到的 latest_rtcm 不同；保留的历史转发样本用原 run_sequence 标明所属轮次。

这里内层 `rtcm.age_ms` 是开始发送时的源帧年龄，保留当时排队情况，不再增长；外层 `latest_transfer.age_ms` 是距本次发送开始的时间，随快照更新。独立 `latest_rtcm.age_ms` 则始终是当前源帧的接收年龄。

每片包含 `message_id=233`、`flags`、`fragmented`、`fragment_id`、`sequence_id`、`length_bytes`、`payload_hex`、`wire_hex`、`wire_length_bytes`、`write_succeeded=true`。payload_hex 仅含 len 指定的有效改正字节；wire_hex 是实际成功写入3DR串口的完整MAVLink帧，含头和校验。

flags的bit0为分片标志，bit1..2是片号0..3，bit3..7是组序号0..31。每片最多180字节；≤180字节可不分片；360/540这类整倍数组需额外零长度尾片；720字节四满片即完整。超过720字节的合法RTCM帧按连续传输组发送、使用新组序号，不截断。该兼容策略依据PX4 GNSS注入字节流实现，目标固件室外接收效果仍须实测。

三个序号含义不同：frame_sequence是Pi收到的完整RTCM帧编号，sequence_id是5位MAVLink改正分组号，MAVLink线上帧头另有8位发送seq。MAVLink2以FD开头，10字节头中seq在偏移4、sysid在5、compid在6、三字节消息号在7..9；消息233的负载开头是flags、len，再跟data。MAVLink2可省去负载尾部零字节，所以wire长度可能较短；payload_hex仍按消息len还原有效改正字节，不能简单把线上剩余字节全部拼为RTCM。

`complete=true` 只在该帧所有分组/分片写入成功后置位；写失败保留已成功片并为false。GPS_RTCM_DATA没有逐帧接收ACK。后续收到Fixed也不等于逐帧ACK，须分别展示。

### 4.4 本地转发状态

`relay` 包含：

- `state`：stopped、paused、waiting_base、waiting_px4、waiting_rtcm、forwarding。forwarding表示当前允许转发且近期有RTCM，实际写入量看计数。
- `requested`：用户是否请求运行；等待状态也可能为true。`run_sequence/run_elapsed_ms` 表示本进程第几轮及从开始以来的时间（含暂停），停止后计时冻结，新开始归零。
- `received_frames/forwarded_frames`：服务看到的合法源帧数、全部片写成功的完整帧数。
- `sent_fragments/sent_payload_bytes/sent_wire_bytes`：成功发送片数、有效改正字节、MAVLink线上字节；不等于无线空中实际吞吐或对端收到字节。
- `pending_frames/queue_capacity=32/max_frame_age_ms=1000`：待发队列和时效。每轮主循环最多发1个完整RTCM帧，先处理遥测/心跳；不重放超过1秒的待发数据。
- `dropped_inactive/dropped_unready/dropped_stale/dropped_overflow`：停止/暂停或激活边界隔离、条件不足、排队过期、队满淘汰；基站入口另有 `base.rtcm.queue_dropped`，坏CRC只记 `base.rtcm.crc_errors`。
- `send_failures/last_error/last_sent_age_ms`：改正写入失败和距最后成功完整转发的年龄。
- `delivery_evidence="local_serial_write_only"`、`receiver_ack_available=false`：证据边界。

### 4.5 移动站定位样本

`px4.gps_observation` 独立于 `px4.frame_age_ms`（后者含心跳等帧）。

| 字段 | 语义 |
|---|---|
| `known/stale` | 是否收到当前会话GPS_RAW_INT；新样本3秒未更新即过期 |
| `sample_sequence` | 每个新非零源时间加一；重复同一源时间只更新接收年龄。源时间0明确按接收时点更新 |
| `frame_age_ms/sample_age_ms/source_time_usec` | GPS帧接收年龄、最后新测量年龄、飞控消息内原时间；源时间可能是开机时间或UTC，不能随意当Unix时间 |
| `fix_type/last_fix_type/fix_name` | 当前状态（过期为null）、最后已知类型、名称；5为rtk_float，6为rtk_fixed |
| `satellites_visible` | 255未知转null；无定位时的0如实保留 |
| `lat_deg/lon_deg/alt_m` | 新鲜且至少3D定位时的位置；高度为GPS_RAW_INT.alt海拔；否则null |
| `h_acc_m/v_acc_m/accuracy_kind` | 接收机估计精度，单位m；未提供/0/未知或无有效3D定位为null。accuracy_kind固定receiver_estimate |
| `raw_hex` | 现有唯一读取者实际收到、通过MAVLink校验、与当前接受的测量字段对应的完整GPS_RAW_INT帧；重复源时间不替换它 |
| `transitions[]/transitions_evicted` | 从最近16次连接内变化记录中筛选本轮实验的变化；项为sample_sequence/fix_type/age_ms/run_sequence/run_elapsed_ms；连接内淘汰量单独计数 |
| `transition_history_scope` | current_run；新轮次不携带之前的变化，停止后不增加本轮变化；实时GPS观测继续 |

过期时 `px4.telemetry.gps` 也省略，避免消费旧接口的页面把旧Fixed当当前值。失联后GPS证据和历史清空，重连重新核验同一PX4身份。`px4.firmware` 仅在当前会话实际收到AUTOPILOT_VERSION后提供 flight_sw_version/middleware_sw_version/os_sw_version/board_version/vendor_id/product_id，否则null；不是用版本UID注册设备。

Pi在身份核验后请求一次 GPS_RAW_INT 1Hz 和版本，沿用现有GCS心跳和Basic ID请求。不把发送间隔请求视为飞控已生效；以实际样本年龄判定。常规遥测和改正共用唯一写者及同一MAVLink线上序号。

## 5. 服务器和页面需要接什么

现有服务器源码接收整个嵌套 `telemetry.rtk`，新增字段不要求另一个设备ID。需要服务器工作线将5个操作加入实验目录/租约动作入口，将快照中的样本/状态通过现有设备接口交给网页，并按实验session持续保存新样本和操作事件。当前设备latest快照不能代替完整过程记录。

页面建议按链路并列展示“原帧→字段→状态”，定位曲线以新 `sample_sequence` 采样，按 age/序号识别重复快照。失效/序号重置分段，不能用旧Fixed补曲线；不同进程和重连历史不要直接拼成一条连续测量。暂停和恢复时在曲线上标事件。源帧样本被替换、样本缺失、估计精度未提供都应显示原因。

现有第一实验本地预览不是真实设备页面。本轮不修改服务器/Web，真实网页端实验验收需后续接入。实际室内构建、部署、链路结果另见同日期验证记录。

协议依据：[MAVLink GPS_RTCM_DATA](https://mavlink.io/en/messages/common.html#GPS_RTCM_DATA)、[GPS_RAW_INT](https://mavlink.io/en/messages/common.html#GPS_RAW_INT)、[PX4 RTK](https://docs.px4.io/main/en/advanced/rtk_gps)、[PX4 v1.15 GPS注入](https://github.com/PX4/PX4-Autopilot/blob/v1.15.4/src/drivers/gps/gps.cpp)、[当前CorrectionFramer](https://github.com/PX4/PX4-Autopilot/blob/main/src/lib/gnss/correction_framer.h)。
