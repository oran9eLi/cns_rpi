# 2026-11-06 RTK 第一实验：箱子缓存身份室内验证

本轮验证的是纠正后的单一 CNS 箱身份实现。RTK 已接入原 F407 主流程，9Ps 与 3DR/PX4 各自使用独立串口；PX4 仅提供实验观测，不建立独立 MQTT 设备。实体 F407 尚未连接，按用户授权使用已有合法箱子绑定验证 RTK 上云。

## 源码与运行范围

- 分支：`codex/rtk-base-station`，基于 main `d4c1dbc9b8a1049271843d7d2b7e0e18d906691b`。
- 树莓派：`dcdw@192.168.0.244`，ARM64；本轮通过 Wi-Fi 接入云端，不代表 5G 验收。
- 箱子绑定：原 `/var/lib/cns-rpi/device_binding.json` 中的 `DCDWCNS1GHC0G6LF8MY6`、`device_type=cns_box`。测试复制真实绑定到隔离目录，未改写原绑定。
- 基站：C-RTK 9Ps，实际 `ZED-F9P / HPG 1.50 / PROTVER 27.50`，USB 115200。
- 数传：CP2102 地面 3DR，57600；收到 PX4 system 1/component 1，Basic ID 为 `393233510c004b0031`，仅出现在 `rtk.px4` 中。
- 隔离目录：`/home/dcdw/cns-rpi-rtk-base-20261106`。临时停止原服务释放端口后运行新程序，退出时恢复原服务。

本轮 ARM64 程序 SHA256：`f66e617a7bbff24f54ec072a70c1746bb5bdc58c45f3854357c3455cfafe69c4`。未替换正式程序、未修改正式配置、未提交或推送。

## 构建和协议回归

| 检查 | 实际结果 | 证据 |
| --- | --- | --- |
| WSL g++-15 全量构建与 CTest | 68/68 通过，19.82 秒；本轮构建无编译警告 | [CTest 日志](2026-11-06-rtk-box-ctest.log) |
| Pi GCC 14 ARM64 增量构建 | 构建成功，本轮 0 warnings | [构建日志](2026-11-06-rtk-box/build.log) |
| Pi 当前 8 项相关 CTest | 8/8 通过，1.01 秒 | [原生测试日志](2026-11-06-rtk-box/box-native-tests.log) |
| 基站驱动真实 PTY | 24 个用例、482 项断言通过 | 对应 `test_rtk_base_station` |
| 附属 RTK 服务真实 PTY | 4 个用例、75 项断言通过 | 对应 `test_rtk_experiment_service` |
| F407 自动发现端口排除 | 20 个用例、179 项断言通过，包含实际符号链接 | 对应 `test_mavlink_port_discovery` |
| 真实主程序、三条 PTY、隔离 Mosquitto | 旧入口先 RED，新实现 GREEN；包含在线、缓存身份、身份冲突和未绑定场景 | 对应 `test_rtk_base_gateway.py`，模拟身份只进入隔离 Broker |

三 PTY 进程测试确认：仅箱子注册 `cns_box`；PX4 ID/遥测只下挂；PX4 失联不关闭箱子 MQTT，基站查询仍完成；缓存箱子身份可查询及配置基站；接入不同 F407 身份产生 `conflict` 后停止 RTK 云帧且不改绑定；无箱子绑定时本地基站与 PX4 观测继续，但不使用 PX4 注册。保留 RAM 层、参数读回、重复动作去重、新导航历元停止确认及过期迟到 ACK 取消检查。

此测试使用合法下限 `stale_after_ms=1000`。原固定 1Hz 查询会令正常版本证据在配置期间过期，已通过真实 PTY 先复现再修复；查询间隔现为 `min(1000ms, stale_after_ms/2)`，默认 3000ms 仍为 1Hz，下限 1000ms 为 2Hz，没有放宽新鲜度判定。

服务器现有 v3 快照解析器要求 `drone_id.basic_id` 为对象。缓存 RTK 快照保留精确空结构 `{"drone_id":{"basic_id":{}}}`，不填入 F407 的 Basic ID 观测字段；实时帧不增加该结构。直接链接现有服务器解析实现的 probe 已验证“缺对象拒绝，补空对象接受且内容不变”，见 [合同探针输出](2026-11-06-rtk-box/server-contract-probe.log)。服务器源码与部署均未修改。

## 真实缓存箱子身份 MQTT 与 API

最终复验发生于 `2026-11-06T06:26:32.085Z` 至 `06:26:35.553Z`（北京时间 14:26）。[MQTT 汇总](2026-11-06-rtk-box/box-cached-v3/mqtt/summary.json)、[消息原文](2026-11-06-rtk-box/box-cached-v3/mqtt/messages.jsonl)和[请求原文](2026-11-06-rtk-box/box-cached-v3/mqtt/requests.jsonl)保留真实关联 ID、时刻及回执。

| 观察项 | 实际结果 |
| --- | --- |
| MQTT 唯一业务身份 | `DCDWCNS1GHC0G6LF8MY6`，注册 `cns_box` 并含 `rtk_base` 能力 |
| RTK 遥测 | 收到 37 帧，均沿箱子 Topic/外层身份，PX4 观测也持续更新 |
| 箱子状态 | `identity_status=cached`、`box_identity_verified=false`；未冒充 F407 已连接或业务在线 |
| 普通 F407 数据 | 未出现旧传感值、模块、告警或日志；快照只保留上述空 Basic ID 容器 |
| `rtk_base_status` | `completed`，回执带当前真实基站状态 |
| `rtk_base_survey_in` | `accepted` → `completed`；RAM 读回为 `verified`、60 秒/2 米 |
| PX4 独立注册 | 未收到新的独立在线注册；另记录 1 条旧方案遗留的 retained 注册，不计为本轮注册 |
| 基站后续短观测 | 8 条 1Hz 本地状态样本，基站连接与 PX4 实时身份正常 |
| Survey-in 测量结果 | `active=true`、`valid=false`、`completed=false`；室内没有有效 ECEF 坐标，不作测量成功结论 |

正式设备 API `GET /api/devices/DCDWCNS1GHC0G6LF8MY6` 返回 HTTP 200，`telemetry_received_at=2026-11-06T06:26:39.921Z`。最新快照含 `rtk.base.configuration.state=verified`、60 秒/2 米及 PX4 子 ID；普通遥测对象仅含 `rtk`、`cellular_5g`。见[API RTK 摘录](2026-11-06-rtk-box/box-cached-v3/server-rtk-extract.json)。摘录不含认证配置，保留原响应 SHA256 供核对。

[8 条短观测](2026-11-06-rtk-box/box-cached-v3/stability.jsonl)只能证明该窗口内状态持续，不能作为长时间稳定性验收。配置 `completed` 仅表示 ACK 和 RAM 参数读回完成，不表示 Survey-in 测量完成或移动站 RTK Fixed。

## 恢复与待验

退出后原 `cns-rpi.service` 恢复 `active`。正式程序 `/home/dcdw/cns_rpi/build/cns_rpi`、正式配置 `/var/lib/cns-rpi/config.json` 和原箱子绑定文件前后 SHA256 均一致，[基线摘要](2026-11-06-rtk-box/box-cached-v3/production-before.sha256)保留；本轮新程序仅在隔离联调期间运行。

尚未完成：F407 接回后的实体三链路同时运行、真实 G2 视频并行、室外 Survey-in 测量成功、长时间与断网恢复测试，以及网页 RTK 实验操作页。第一实验当前不转发 RTCM，不包含第二实验的移动站 Fixed；既有原始 RTCM 计数或室内配置成功不能替代这些验收。
