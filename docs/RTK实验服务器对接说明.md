# RTK实验服务器与Web对接说明

版本：2026-11-09。适用：当前已部署的树莓派RTK基站、移动站及教学数据接口。

本文可单独交给服务器和Web开发人员，包含两项实验的操作、数据、兼容改动和验收要求，无需先阅读聊天记录。下述Pi协议是现有实现；服务器改动清单和页面组织是待实现要求，不代表已经开发或部署。

## 1. 责任范围与当前进度

| 部分 | 已有能力／需要完成的工作 |
|---|---|
| 树莓派 | 连接9Ps基站与地面3DR；识别PX4；配置基站Survey-in；解析UBX/RTCM/MAVLink；显式开始、暂停、恢复、停止改正转发；提供真实原文、状态及计数。 |
| 服务器 | 补实验目录、会话与动作协议，适配路由和回执，保存过程数据，向网页提供状态、历史、操作结果。 |
| Web | 沿用现有实验工作台，提供基站架设、移动站固定解两项实验；展示步骤、链路、参数、测量／收敛过程、真实报文和操作记录。 |

现场设备：C-RTK 9Ps基站（已识别ZED-F9P）、C-RTK3 G5移动站、雷迅X25 Evo/PX4、3DR数传。相机实验独立，不作为RTK实验的前置条件。RTK链路不依赖QGC。

11月9日已完成：Pi部署、缓存箱体身份上云、真实基站/PX4原帧上云、五项移动站操作往返；用户现场分别拔插基站和地面3DR，云端正确显示失联、清除旧证据、自动恢复，另一路与上云保持正常。无F407开发板，使用合法缓存身份。

尚未验收：室外Survey-in真正完成、真实改正经无线链路被移动站采用、Float/Fixed与暂停恢复过程、F407接回后的共存、完整Web实验。室内RTCM为0，不能以软件分片测试代替实际无线RTK验收。本轮仅整理文档，没有修改服务器代码。

## 2. 两项实验的流程与通过依据

### 2.1 RTK基站架设

1. 检查箱体上云、基站USB与接收机有效响应，分别展示数传/PX4状态。
2. 学生填写最低测量时长、精度门限，提交`rtk_base_survey_in`。
3. 展示操作`accepted`及最终配置回执；`configuration.state=verified`表示RAM读回匹配。
4. 继续观察真实测量时长、平均精度、观测数、ECEF坐标、active/valid。
5. 以新鲜的`base.survey_in.completed=true`判定基站测量完成，再观察合法RTCM计数、类型和新鲜度。

最低时长是条件之一，不能显示成“倒计时结束必成功”。配置动作完成、基站测量完成、移动站Fixed是三个不同结果。室内无定位、坐标为空和RTCM为0均按真实情况显示。

### 2.2 RTK移动站固定解

1. 检查基站与PX4通信，开始前记录移动站实际定位状态。
2. 发`rtk_rover_start`；允许先进入等待，条件满足后Pi转发新改正。
3. 对照RTCM原帧、实际MAVLink发送分片与GPS状态，绘制真实定位状态／估计精度随时间变化。
4. 发`rtk_rover_pause`，记录是否退化及耗时；发`rtk_rover_resume`，记录恢复过程。
5. 结束时发`rtk_rover_stop`并检查回执，保存实验记录。

GPS的`fix_type=5/6`分别表示Float/Fixed。不得预设一定经过Float、必然Fixed、暂停后立刻退化。接收机估计精度不等于经实测确认的绝对定位误差。静置无人机可以做上述地面观测，不要求起飞。

## 3. 唯一设备身份与连接

- MQTT业务设备始终是`device_type=cns_box`的箱体，基站和PX4属于该箱体的实验链路。
- 无F407时允许使用已有合法缓存绑定。`telemetry.rtk.box_identity_verified=false`应显示“使用已保存的箱体身份”，不应因此阻止RTK实验，也不能显示为F407实时身份已核验。
- 不以PX4 ID、接收机序列号或Pi序列号另注册业务设备；已有身份冲突应保留原有阻止控制的规则。
- 缓存身份快照根级保留`drone_id.basic_id={}`，不携带旧F407普通传感数据；该空对象不表示实时身份观测。

当前联调用箱体ID：`DCDWCNS1GHC0G6LF8MY6`。已核验的PX4附属ID：`393233510c004b0031`。这些是现场样例，产品代码从实验会话选择的箱体取得ID。

已验证的设备数据入口：

```text
GET http://112.124.52.232/api/devices/DCDWCNS1GHC0G6LF8MY6
```

读取`item.latest_telemetry.telemetry.rtk`，整份快照的服务器接收时间为`item.telemetry_received_at`，设备发送时间在`item.latest_telemetry.sent_at`。实际请求沿用平台现有访问方式。MQTT认证继续使用现有部署配置，不在本文或浏览器中放入认证信息。

## 4. MQTT通道与平台操作路径

`{namespace}`使用现有部署配置，仓库示例为`cns_rpi`。

| 用途 | Topic | 当前默认 |
|---|---|---|
| 注册 | `{namespace}/{箱体ID}/registration` | schema 3，QoS 2，retain |
| 低频完整快照 | `{namespace}/{箱体ID}/telemetry/snapshot/v1` | schema 3，约1Hz，QoS 0 |
| 高频精简遥测 | `{namespace}/{箱体ID}/telemetry/realtime/v1` | schema 1，约10Hz，QoS 0 |
| 下发到Pi | `{namespace}/{箱体ID}/experiment/set` | schema 1，QoS 2，不retain |
| Pi动作回执 | `{namespace}/{箱体ID}/experiment/ack` | schema 1，QoS 2，不retain |

平台正式操作应沿用“浏览器 → 后端实验Session/动作API → 现有`web-console`来源路由请求 → route_service → Pi的`experiment/set` → 回执”的路径。这里给出的`experiment/set`信封是Pi最终消费的格式，不是浏览器直接发布MQTT的权限。

现有`POST /api/experiment-sessions/:id/actions`等Session接口可作为扩展点，其RTK请求类型、场景、操作权限尚需服务器实现，不应把本文示例当作已部署的REST功能。

## 5. 七项操作与精确请求格式

| operation | parameters | 行为 |
|---|---|---|
| `rtk_base_status` | 不允许出现 | 查询本次基站状态，不开始测量。 |
| `rtk_base_survey_in` | 必须有且仅有下述两项 | 在RAM设置Survey-in和USB输出并读回，不保存到接收机Flash。 |
| `rtk_rover_status` | 不允许出现 | 查询完整RTK状态。 |
| `rtk_rover_start` | 不允许出现 | 从停止状态开始新一轮；已开始时拒绝。 |
| `rtk_rover_pause` | 不允许出现 | 暂停改正、清空待发旧改正；遥测继续；未开始时拒绝。 |
| `rtk_rover_resume` | 不允许出现 | 从暂停恢复；其他状态拒绝；不补发暂停期间旧改正。 |
| `rtk_rover_stop` | 不允许出现 | 停止改正并清队列；重复停止允许成功。 |

**Survey-in线上参数名必须是`minimum_duration_s`和`accuracy_limit_m`。** 前者为10..86400的整数秒，后者为0.1..100的有限数值，单位米。不要误用Pi配置文件／状态中的`survey_min_duration_s`、`survey_accuracy_m`作为下发字段。

普通操作必须正好九个顶层字段，不可附加空`parameters`或任意字段。示例：

```json
{
  "schema_version": 1,
  "command_id": "3f886532-0000-4000-8000-000000000001",
  "request_id": "3f886532-0000-4000-8000-000000000002",
  "target": {"device_id": "DCDWCNS1GHC0G6LF8MY6"},
  "session_id": "3f886532-0000-4000-8000-000000000003",
  "action_id": "3f886532-0000-4000-8000-000000000002",
  "lease_version": 1,
  "operation": "rtk_rover_status",
  "expires_at": "2026-11-09T12:00:30.000Z"
}
```

Survey-in必须正好十个顶层字段：

```json
{
  "schema_version": 1,
  "command_id": "3f886532-0000-4000-8000-000000000004",
  "request_id": "3f886532-0000-4000-8000-000000000005",
  "target": {"device_id": "DCDWCNS1GHC0G6LF8MY6"},
  "session_id": "3f886532-0000-4000-8000-000000000003",
  "action_id": "3f886532-0000-4000-8000-000000000005",
  "lease_version": 1,
  "operation": "rtk_base_survey_in",
  "expires_at": "2026-11-09T12:00:30.000Z",
  "parameters": {"minimum_duration_s": 60, "accuracy_limit_m": 2.0}
}
```

示例UUID和日期只说明格式，不能原样发布。运行时由服务器生成UUID和有效的未来过期时间，格式为UTC毫秒`YYYY-MM-DDTHH:mm:ss.sssZ`，`request_id`必须等于`action_id`，`target`只能含正确箱体的`device_id`。

`lease_version`为正整数，由平台沿用现有独占租约管理；同一最高租约不能被不同session复用，不用时间戳随意抬高租约。每个新动作生成新action ID；重投同一动作只换command ID，其他字段保持不变。只读查询要取新状态，也必须生成新action ID，否则会取得缓存回执。

Pi请求上限仍为4096字节，不能接受任意串口字节、飞控控制命令或额外参数。进程内动作缓存最多128项，终态至少保留300秒，重启不保留；服务器仍负责持久记录和避免重复发起业务动作。

## 6. 回执、错误与状态机

Pi回执固定字段：`schema_version`、`command_id`、`session_id`、`action_id`、`operation`、`observed_at`、`status`、`fact`。出错增加`error_code`。

**回执不含`request_id`、`device_id`、`lease_version`或`target`。** 通过可信Topic和已保存的command/action/session关联，不要求Pi补这些字段。

| status | 平台处理 | 结果内容 |
|---|---|---|
| `accepted` | 进行中，继续等待终态 | 当前基站事实，不是测量完成。 |
| `completed` | 本地操作完成 | 查询完成、配置读回完成或转发控制完成。 |
| `rejected` | 拒绝执行，终态 | `error_code`及诊断事实。 |
| `failed` | 已接受动作执行失败，终态 | `error_code`及诊断事实。 |

两项基站操作用`fact.base`，形状与`telemetry.rtk.base`一致；五项移动站操作用`fact.rtk`，形状与`telemetry.rtk`一致。失败和拒绝也固定包含fact，其中的未知值照实保留，不应只允许成功回执有fact。

Survey-in通常为`accepted → completed/failed`；两种状态可能紧邻到达。不得把accepted标记为业务成功，也不得因已有accepted而丢弃后续合法终态。超时后不自动复用原action ID重做配置；先查询实际状态，再按用户意图创建新动作。

已实现错误包括：`wrong_target`、`action_conflict`、`expired`、`stale_lease`、`action_cache_full`、`configuration_busy`、`base_configuration_rejected`、`base_configuration_failed`、`base_disconnected`、`configuration_timeout`、`rtk_rover_unavailable`、`rtk_rover_rejected`。不能可靠关联的非法JSON／字段请求可能只有Pi诊断日志，没有业务ACK，平台必须保留超时结果。

## 7. 遥测字段与学生可见内容

统一位于`telemetry.rtk`。下面列的是展示所需字段，不应由前端模拟生成。

| 路径 | 含义／展示规则 |
|---|---|
| `box_identity_verified` | 当前F407身份核验事实；false可显示使用缓存箱体身份。 |
| `base.connected` | Pi持有基站串口；不单独证明接收机已响应。 |
| `base.receiver_verified` | 新鲜版本响应验证受支持接收机；不代表测量完成。 |
| `base.protocol` | 型号、固件、协议、错误和新鲜度。 |
| `base.configuration` | `state`、`storage=RAM`、状态字段`survey_min_duration_s/survey_accuracy_m`、`verified_age_ms`；与线上请求参数名有区别。 |
| `base.survey_in` | `known/active/valid/completed/stale`、`duration_s`、`mean_accuracy_m`、`observations`、`ecef_m.x/y/z`、`i_tow_ms`、`sample_sequence`、`last_rx_age_ms/last_sample_age_ms`、`raw_hex`。坐标和精度单位米。 |
| `base.rtcm` | `valid_frames/bytes/message_types/crc_errors/last_rx_age_ms/stale`；观察窗口以新增量和新鲜度判断输出。 |
| `base.last_error` | 当前错误，没有则null。 |
| `px4.serial_connected` | 本地3DR串口已打开；不证明另一端上电或无线链路可用。 |
| `px4.identity_verified/device_id` | 远端PX4身份及通信事实；失联清除，不改变箱体ID。 |
| `px4.frame_age_ms` | 最近有效MAVLink帧年龄；不是无线信号强度，也不能代替GPS样本年龄。 |
| `px4.gps_observation` | 见下表；优先使用此结构呈现定位。 |
| `px4.firmware` | 实际收到版本应答后提供原始版本数字，否则null；不作业务注册。 |
| `relay` | 转发请求、运行／等待／暂停状态、计数与证据边界。 |
| `observation` | 最近真实RTCM和最近一次实际转发尝试，见第8节。 |

GPS字段：

| 字段 | 规则 |
|---|---|
| `known/stale` | 是否有当前连接的GPS测量；新测量3秒未更新为过期。 |
| `sample_sequence/source_time_usec` | 新测量序号／飞控源时间；源时间可能为开机时间，不能无条件当Unix时间。 |
| `frame_age_ms/sample_age_ms` | 分别为GPS帧接收年龄和新测量年龄，单位ms；重复源时间不刷新新测量证据。 |
| `fix_type/last_fix_type/fix_name` | 当前／最后已知状态；过期时当前fix_type为null。0/1无有效定位，2为2D，3为3D，4为DGPS，5为Float，6为Fixed；其他保留实际名称／数字，不强行归类。 |
| `satellites_visible` | 未知为null，0按真实值展示。 |
| `lat_deg/lon_deg/alt_m` | 经纬度为度，高度为GPS_RAW_INT.alt海拔米；新鲜且至少3D时才提供，否则null。 |
| `h_acc_m/v_acc_m/accuracy_kind` | 接收机估计精度，米；未提供、未知或无有效3D定位为null，kind为`receiver_estimate`。 |
| `raw_hex` | 对应当前接受测量的实际完整GPS_RAW_INT帧，不能用旧帧补新测量。 |
| `transitions/transition_history_scope` | 本轮定位变化，scope为`current_run`；项含`sample_sequence/fix_type/age_ms/run_sequence/run_elapsed_ms`，来自有限的最近16次连接内变化。 |
| `transitions_evicted` | 当前连接历史淘汰量；不代表服务端已有完整历史。 |

### 7.1 转发状态

| `relay.state` | 含义 |
|---|---|
| `stopped` | 未请求转发／已停止。 |
| `paused` | 暂停；遥测仍继续。 |
| `waiting_base` | 等待新鲜、已完成测量的基站。 |
| `waiting_px4` | 等待有效PX4通信；只有基站条件已满足才会显示此项。 |
| `waiting_rtcm` | 其他条件满足，等待新改正。 |
| `forwarding` | 当前允许转发且近期有RTCM；实际写入量仍看发送计数。 |

等待原因按基站→PX4→RTCM顺序判定；基站未就绪时拔掉3DR，顶层仍可能是waiting_base。网页必须分别展示独立链路状态。

其他字段：`requested`、`run_sequence/run_elapsed_ms`、`received_frames/forwarded_frames`、`sent_fragments/sent_payload_bytes/sent_wire_bytes`、`pending_frames/queue_capacity/max_frame_age_ms`、`dropped_inactive/dropped_unready/dropped_stale/dropped_overflow`、`send_failures/last_error/last_sent_age_ms`。

本地待发队列32帧，源帧转发时效1秒；暂停、重新就绪和激活边界不补发旧改正。`run_elapsed_ms`含暂停时间，停止后冻结，新开始归零。进程重启后轮次、计数重置；基站连接计数也可能因重连重置，不能假设累计值永久递增。

`delivery_evidence=local_serial_write_only`、`receiver_ack_available=false`。发送字节数只证明写入本地数传串口，不证明无线送达或移动站采用。

## 8. 真实协议教学窗口与带宽

完整原文进入约1Hz的snapshot、动作回执和Pi本地状态文件；高频realtime省略`base.survey_in.raw_hex`、`px4.gps_observation.raw_hex/transitions`、`observation.latest_rtcm/latest_transfer`，并以`samples_included=false`表明。

高频省略字段不等于清空。原文窗口以完整快照更新；完整快照明确给出的null／空值、失联和过期需要如实处理。当前Node实时gateway未订阅完整snapshot；服务器可以补该订阅，也可以先从现有设备GET读取，不能把只有realtime的订阅当作完整原帧通道。

| 教学窗口 | 数据与可解释内容 |
|---|---|
| UBX基站状态 | `base.survey_in.raw_hex`：真实NAV-SVIN完整帧，对照测量字段。 |
| RTCM源帧 | `observation.latest_rtcm`：`frame_sequence/message_type/purpose/length_bytes/crc_valid/age_ms/raw_hex`，原文含完整帧头和CRC。 |
| MAVLink发送分片 | `observation.latest_transfer`：`run_sequence/rtcm/fragments/complete/age_ms/evidence`；每片含`message_id=233/flags/fragmented/fragment_id/sequence_id/length_bytes/payload_hex/wire_hex/wire_length_bytes/write_succeeded`。 |
| GPS定位原帧 | `px4.gps_observation.raw_hex`：真实GPS_RAW_INT，对照fix、卫星、位置和估计精度。 |

`observation.capture_mode=bounded_latest`、`complete_capture=false`，只保留最近一个RTCM和最近一次转发尝试，不是全量抓包或每种消息各保留一条。`rtcm_samples_replaced`记录源样本替换数。十六进制实际可为小写，入口应兼容合法大小写。

RTCM3以D3开头，负载首12位是消息类型，末3字节是CRC24Q。1005为参考站坐标，1077/1087/1097/1127分别为GPS/GLONASS/Galileo/北斗MSM7，1230为GLONASS偏差信息；观测量消息不能称为无人机坐标。Pi只提供帧封装／类型识别，不提供完整RTCM观测量解算。

GPS_RTCM_DATA每片最多180字节；组序号为5位、片号为2位。360/540字节这类情况有额外零长度尾片，720字节四满片可完成；大于720字节的完整RTCM按连续分组发送。页面显示实际片，不按固定片数造数据。MAVLink2会压缩尾部零字节，线上`wire_hex`短不一定是缺帧。

`latest_transfer.rtcm.frame_sequence`关联本次源帧；`latest_rtcm`可能已被后续新帧替换。内层源帧`age_ms`冻结为发送开始时的年龄，外层转发`age_ms`随时间增长。`complete=true`仅表示该次完整源帧所有片本地写成功，GPS_RTCM_DATA没有逐帧接收ACK。

## 9. Web页面与实验记录要求

- 沿用现有工作台：左侧实验步骤、顶部独立链路状态、中间参数／观测、原始报文窗口、操作历史。现有RTK本地预览可参考布局，其USB接入按钮、模拟曲线、虚构坐标、加速时间不能混入真机模式。
- 先交付第一实验的连接→配置→测量进度→RTCM输出；第二实验增加开始／暂停／恢复／停止、真实状态变化和估计精度曲线。
- 同时显示服务器接收时间、Pi样本年龄与过期状态。前端网络断开后，已缓存的stale=false不能一直显示为新鲜；阈值应考虑现有服务器写库周期，不能仅用发送源时钟或过短固定阈值误报。
- 按新的`sample_sequence`采样，记录实际接收时间；重复快照不生成伪测量。序号重置、失联、进程重启和未知值应分段显示，不能用旧Fixed填补曲线。
- 保存实验Session、设备ID、轮次、学生参数、实际读回参数、测量摘要／坐标、GPS状态与估计精度、RTCM窗口增量、转发计数、原帧关联、操作/回执与学生结论。保留失败和“未达到Fixed”等真实结果。
- 原始帧是最近样本；服务端历史必须说明采样频率、窗口、截断和缺失，不能把抽样历史称为全量抓包。
- 若保留现有基站预览中的参数对照／坐标重复性步骤，数据必须来自已保存的独立真实轮次，不强行规定参数变化必然带来某种结果。重复性不等于绝对定位精度。

## 10. 服务器代码兼容改动清单

以下根据11月9日本地`cns_server`源码只读核对，实施前复核团队当前分支；未执行这些修改。

| 位置 | 需要完成的兼容点 |
|---|---|
| `backend_service/protocol/src/experiments.ts`及RTK协议模块 | 加RTK场景、七operation、精确parameters、base/rtk fact及四种Pi回执；更新REST动作与路由请求类型。 |
| `backend_service/src/experiments/catalog.ts`与Session服务 | 增加两实验／步骤，按箱体RTK能力筛选；沿用token、设备租约和独占规则，允许合法cached身份。能力标识为`rtk_base/rtk_rover/rtk_protocol_observation`，有能力不等于当前在线或已Fixed。 |
| `experimentActionService/Store/Dispatcher` | 保存并传递Survey-in参数，维护重试身份与回执状态；不把accepted当终态。 |
| `route_service/src/core/command/source_request.cpp` | 扩展操作白名单、Survey-in字段、四种ACK状态与fact解析；保留原有来源、目标、身份约束。 |
| `command_types.hpp`、`experiment_policy.cpp` | 在最终Pi信封中保留精确parameters，仅Survey-in带参数。 |
| `command_state.cpp`、`core/runtime/command_service.cpp` | accepted映射进行中，completed映射成功，failed/rejected映射失败；先到accepted、后到终态不能被误判为冲突或丢弃。 |
| 数据库迁移与事件记录 | 扩展operation/状态约束，处理当前非成功动作fact必须为空的约束；可调整约束或把失败诊断事实保存为事件，不得直接丢弃。保存真实采样和操作历史。自动生成的MySQL结构从迁移源生成，不手改生成结果。 |
| MQTT gateway／设备快照与实验视图 | 提供完整snapshot原帧、接收时间和历史，不只接高频realtime；后端入口校验后以明确类型传给页面。 |
| 前端实验中心、准备页、工作台和会话生命周期 | 加两实验路由与stage、参数/状态/报文/历史视图，沿用租约和离开提醒；演示与真机数据明确区分。 |

**报文大小：** 当前Node协议和C++路由设备回执存在4096字节限制，RTK完整原帧加多片wire_hex可能超出。建议仅对RTK富回执提供明确的64KiB上限，并贯通MQTT接入、解析、转发、存储与前端响应；其他实验及Pi下发请求仍保留原限制。64KiB是服务端适配建议，不是已部署或Pi请求上限。用真实最大样本核验是否完整保存，不能截断后冒充完整帧。

## 11. 结束、重启与失联行为

- Pi进程重启后改正默认`stopped`，不会自动重放Survey-in配置；设备重新上电后先查询，再按实验步骤显式发起测量。
- 接收机的Survey-in配置只在RAM；仅重启Pi程序时，基站接收机自身仍可能继续已有测量，Pi不沿用旧配置核验结论。
- 基站重新测量会把转发置为paused，需要显式恢复。开始／恢复／链路重新就绪后只发送新改正。
- MQTT短暂断开不会自动停止已开始的本地转发。动作`expires_at`不表示整个实验或持续转发将在该时刻自动停止。
- 第二实验结束需显式发送`rtk_rover_stop`并展示是否确认；网页退出或Session结束本身不能冒充设备已停止。
- 当前没有`rtk_base_stop`接口。结束基站网页会话不等于停止接收机测量，不应增加一个只改变网页状态的“已停止测量”结论。

## 12. 对接验收表

| 场景 | 通过依据 |
|---|---|
| 无F407、合法缓存身份 | 可进入实验、上报和执行RTK动作；仅一个箱体业务ID，身份标识如实为false。 |
| 室内正常接线 | 基站响应、PX4身份、GPS真实原帧与云端时间更新；允许无定位、RTCM为0。 |
| 基站USB拔出／插回 | 云端base断开、旧证据清除，PX4与上云继续；插回自动核验并恢复新数据。 |
| 地面3DR USB拔出／插回 | PX4身份和GPS证据清除，base与上云继续；插回重新核验并恢复。 |
| 配置操作 | accepted之后有终态及实际RAM读回；网页不把completed当Survey-in完成。 |
| 移动站操作 | 当前室内常见`stopped → waiting_base → paused → waiting_base → stopped`，五项操作回执可关联；不据此声称已转发真实改正。 |
| 富回执与原帧 | 超过4KB的合法RTK回执完整通过，旧实验仍按原上限；高频省略原文不清空快照，真实失联/null正确呈现。 |
| 断网／重新加载页面 | 快照会随时间过期；历史可回读、重复样本不造曲线；运行转发与页面连接状态分别显示。 |
| 室外基站 | 真实测量完成，新鲜合法RTCM持续产生；保留参数、测量过程和输出窗口。 |
| 室外移动站 | 保存实际定位状态、原始数据、改正发送、暂停／恢复事件；Fixed或未Fixed均可如实记录，不制造预设过程。 |

推荐交付顺序：真实只读状态与原帧 → 基站配置及回执 → 基站历史记录 → 移动站控制、曲线与报告。每一步标明已实现、已部署和已真机验证的范围。

## 13. 可选源码追溯

本文已包含对接合同；需要核对实现时，可查Pi仓库`src/experiment/rtk_base_request.cpp`、`rtk_base_experiment.cpp`、`src/rtk/base_station.cpp`、`experiment_service.cpp`、`px4_session.cpp`和`rtcm_encoder.cpp`。原分项文档及验证日志是补充资料，不是阅读本文的前置条件。

服务器部署地址、认证和操作者权限由服务器团队沿用现场配置；本文不携带账号、密码或热点信息。
