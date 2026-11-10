# RTK 基站架设实验：树莓派端设计

用户已确认先实现第一项 RTK 基站架设实验，使用 9Ps、3DR 和 PX4 在室内验证后再搬运设备到室外。场景已纠正为学生使用 CNS 箱内 Pi：F407、3DR/PX4 和 9Ps 各用独立串口，唯一 MQTT 身份属于箱子 F407。随后用户授权 F407 暂未连接时，使用已有持久化箱子身份上报 RTK。基线是 2026-11-06 获取的 origin/main `d4c1dbc`，开发分支 `codex/rtk-base-station`。

## 目标与边界

- 用实际 USB 端口读取 9Ps 身份、Survey-in 和 RTCM 状态，必要时显式开始观测。
- 状态沿 F407 提供的合法持久化箱子 `device_id` 上云；PX4 Basic ID 仅作实验元数据，不注册第二个设备。
- 保留现有 F407、G2 和维护流程；RTK 默认关闭，启用时在同一主循环增加两条链路，不替换 F407 流程。
- 有持久化 `cns_box` 绑定且无身份冲突时，允许缓存身份的 RTK 帧；F407 未实时业务在线时不在新帧中携带其普通传感数据。运行三态如实报告 cached/offline/unknown，`telemetry.rtk.box_identity_verified` 明示是否有当前 F407 核验。
- 没有合法箱子绑定时，仅保留本地 base/PX4 观测，不借用 PX4 身份注册 MQTT。
- 首版不实现移动站固定解实验、RTCM 到飞控的注入、QGC 桥接或 F407 改动。
- 室内验证设备、协议、命令及 MQTT 链路，不把无卫星判为程序失败，也不把回放或收到 RTCM 当作真实基站收敛。

## 方案选择

采用单个 CNS 箱 MQTT 会话，在现有 `main` 的组合根接入可选 RTK 模块。F407 `DeviceSession`、持久绑定、MQTT 运行三态、配置/控制、G2 和维护链路继续使用。PX4 与基站各自保留串口、状态和重连，不写入 F407 的共享 `StateStore`。不恢复旧设备模式或 QGC。

早期独立 PX4 注册方案已被用户的整箱场景纠正；其真实设备和云端证据只保留为历史组件结果，不能代表当前箱子身份、三链路及 G2 共存通过。

基站优先复用已有串口封装。协议适配以 u-blox 官方 F9P USB 配置和 UBX 文档为依据，保留原设置直到显式操作；配置只写 RAM，完成判定必须有 ACK 和读回。若复用完整 PX4 GPSDrivers 需要引入其他传感器接口及同步配置流程，则使用小范围 F9P USB 适配，记录依据和测试，不维护另一套通用 GNSS 驱动。

## 模块与接口

- `rtk/base_station`：单一基站串口所有者、混合流解析、观测配置及 ACK/读回、按接收时间判断新鲜度。
- `rtk/px4_session`：PX4 心跳端点、每两秒 Basic ID 请求、实验身份核验、来源过滤及断链失效；其身份不参与 MQTT 寻址。
- `experiment/rtk_base_request`：在外部 JSON 入口验证实验信封与观测参数，返回类型化请求。
- `rtk/experiment_service`：作为主流程中的附加模块，管理基站与 PX4 链路及 RTK 事实，不创建第二个 MQTT 客户端；组合根负责写本地状态。
- `main`：沿箱子绑定统一注册/发布，在已有实验分发接入 RTK 操作并共用 ACK outbox。配置取消/超时检查先于基站接收推进，不能因共享队列满而跳过。
- `rtk_base` 配置：默认关闭；`device/baud` 属于 9Ps，新增 `px4_device/px4_baud` 属于 3DR/PX4；这两个设备须用明确且不同的绝对路径，并与 F407 分开。`serial.device/baud` 继续属于 F407，可为 `auto`，发现排除 RTK 两个设备及其别名。

MQTT 沿用箱子 `device_id` 的 `experiment/set`、`experiment/ack` 和既有 snapshot/realtime topic。操作为 `rtk_base_status` 和 `rtk_base_survey_in`；后者参数为 `minimum_duration_s`、`accuracy_limit_m`，`target.device_id` 为箱子 ID。遥测新增 `telemetry.rtk={base,px4,box_identity_verified}`，外层及注册均为箱子 `cns_box`，保留既有能力。F407 业务在线时附加于正常帧；仅缓存箱子身份时只发布 RTK 实验事实，不夹带旧 F407 普通传感数据。服务器源码及服务器实验动作准入不在本次改动范围。

服务器现有 `ParseTelemetry` 要求 schema 3 的根级 `drone_id.basic_id` 两层均为对象。缓存 RTK 快照补精确空容器 `"drone_id":{"basic_id":{}}`，不填 `id_type`、`ua_type` 或任何 F407 实时观测值；schema 1 实时帧仍无 `drone_id`。该容器只满足接收结构，当前身份核验仍由 `rtk.box_identity_verified` 表达，服务器代码不改。

本地状态含 `box:{device_id,identity_verified}`、`base` 与 `px4`。本地和 MQTT 都区分箱子缓存/当前核验、端口打开、接收机已响应、配置已验证、Survey-in 正在进行/有效、RTCM 收到、新鲜/过期。PX4 断链后不保留其“已实时核验”结论，也不关闭箱子 MQTT；只有真实接收样本改变观测状态。

## 验证与现场交付

先完成原 main 的构建/CTest 基线，再对协议、三端口配置/排除、身份隔离、命令和三条伪串口加 Broker 的真实进程做测试。覆盖 F407 在线、缓存箱子身份、无绑定、身份冲突、PX4 断开及 G2 共存；缓存帧必须验证无旧 F407 普通传感数据，schema 3 保留精确空 `basic_id` 容器而 schema 1 不增加该字段。实体 Pi 使用隔离暂存构建与限时运行，保留生产配置和服务备份；不上传模拟身份到生产 Broker。真实云端上报只使用实际 F407 箱子绑定，单独记录 MQTT 和箱子 ID 的 HTTP 快照持久化。

本轮 WSL 全量 68/68、Pi ARM64 相关测试 8/8、增量构建 0 条警告；F407 未接时，合法缓存箱子身份的 RTK MQTT、状态查询、Survey-in RAM 配置及正式 HTTP 快照持久化已取得实测证据，详见 [缓存箱子身份验证记录](../../validation/2026-11-06-rtk-box-cached.md)。正式服务已恢复且正式程序、配置、绑定摘要未变。该结果不替代 F407 接回后的实体三链路/G2 同时运行验收；室外 Survey-in 成功、移动站 Fixed、定位精度、纯移动供电及网页实验页面仍未验或未做。旧独立 PX4 证据只保留为历史组件结果。
