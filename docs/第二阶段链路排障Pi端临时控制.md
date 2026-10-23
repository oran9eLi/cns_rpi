# 第二阶段链路排障实验：Pi 端临时控制

状态：Pi 端实现、本地与实体 Pi 自动化测试及服务启动已完成；四类命令、F407 屏幕和 Server/Web 闭环尚待现场联合验收。本文仅描述设备端行为，不替代 Server 仓库的《第二阶段里程碑四链路连通与故障定位实验设计》。

## 接收与应答

只有已持久化绑定的主控箱订阅 `{namespace}/{device_id}/experiment/set`，订阅 QoS 2；ACK 发往同设备 `experiment/ack`，QoS 2、`retain=false`。沿用现有 MQTT 完成回调与待发队列，发布失败保留原 ACK 原文等待重连重发。动作不改变 registration、运行三态、遥测 Topic、真实 5G 拨号和 MQTT 控制链。

请求固定包含 `schema_version=1`、`command_id`、`request_id=action_id`、`target.device_id`、`session_id`、`action_id`、正整数 `lease_version`、`operation` 和 UTC 毫秒 `expires_at`。各操作另有且仅有如下字段：

| 操作 | 附加字段 | 完成 ACK 的 `fact` |
|---|---|---|
| `link_lab_set_cellular` | `state: disconnected\|normal` | `state`、`report_sent` |
| `link_lab_set_baud` | `baud_rate: 57600\|115200` | `baud_rate` |
| `link_lab_set_status_reporting` | `enabled: boolean` | `enabled` |
| `link_lab_restore` | 无 | `restored_baud_rate`、`cellular_override_cleared:true`、`status_reporting_enabled:true` |

ACK 固定包含 `schema_version=1`、`command_id`、`session_id`、`action_id`、`operation`、UTC 毫秒 `observed_at`。成功时为 `status=completed` 并带上述 `fact`；拒绝时为 `status=rejected`，仅带 `error_code`，**不带 `fact`**。错误码限于 `action_expired`、`duplicate_conflict`、`serial_unavailable`、`unsupported_operation` 和 `result_uncertain`。无法安全解析或目标身份不符的载荷不猜测 ACK。

以下为不含现场凭据的格式示例。实际设备 ID、租约版本和时点由 Server 生成，不能照抄示例下发：

```json
{"schema_version":1,"command_id":"7d15e276-c7e1-4f52-aaee-7314a6417667","request_id":"f886f45b-01f8-4a54-b2b6-a65b07477aa1","target":{"device_id":"DCDWCNS1S2MEAG1VTA0C"},"session_id":"c4191037-9e2a-480c-a995-2174e4672563","action_id":"f886f45b-01f8-4a54-b2b6-a65b07477aa1","lease_version":3,"operation":"link_lab_set_cellular","state":"disconnected","expires_at":"2026-10-22T08:00:30.000Z"}
```

成功 ACK 的 `fact` 形如 `{"state":"disconnected","report_sent":true}`；若 UART 写入失败，返回 `status=rejected,error_code=serial_unavailable` 且没有 `fact`。

## 物理事实与恢复

`disconnected` 首次成功必须真实向现有唯一串口完整写入一帧 `RPICELL`，否则拒绝为 `serial_unavailable`。教学覆盖只清除该帧的 `ONLINE` 位，保留接口、载波、IP 和默认路由诊断位；真实快照及遥测中的 5G 数据不变。`report_sent=true` 仅表示 Pi 写入串口，不表示 F407 已接收或屏幕变色。串口失配时执行 `normal` 可以成功，`report_sent=false` 表示本次没有写入状态帧；待 UART 恢复后由周期发送更新屏幕。暂停报送只停止周期 `RPICELL`，不停止心跳、遥测或 MQTT。

临时波特率仅改变当前进程的目标值，使用已核验的串口拓扑路径关闭并重开原有串口，读回 termios 后才完成 ACK。失配期间自动发现固定使用临时值，不偷偷回到持久配置；只有新的命令、恢复、控制链中断、租约超时或进程重启才能解除。串口打开及波特率读回不能证明 F407 已通信，必须另以新的有效 MAVLink 业务帧确认。

Pi 将已接受的最高租约版本与 Session 身份写入绑定文件旁的 `.link-lab-fence.json`。重复 `action_id` 在活动 Session 内回放原终态，不重复物理操作；同 ID 异参返回 `duplicate_conflict`。旧租约不能覆盖新租约；进程重启或控制链中断后临时状态清除，旧 Session 的非恢复动作被拒绝。`link_lab_restore` 幂等，不要求当前 F407 身份已核验：若确实做过临时换速，须重开串口并读回基准速率；若没有临时换速，则清除纯进程覆盖，即使当前串口已拔掉。后一种情况下 `restored_baud_rate` 只表示持久配置的目标值，不是当前无串口时的 termios 实测值。真实 MQTT 控制链断开或本机 300 秒保护期限到达时，本机清除覆盖、封闭旧 Session，并重新按持久波特率发现串口；报送恢复后将尽快按周期发送真实状态，仍须等待真实 F407 身份和串口可用。Server 必须独立核对现场恢复，不能以 Pi ACK 代替 F407 屏幕与业务帧事实。

临时动作的完成 ACK 在本进程内有界缓存并由 MQTT 待发队列重试；跨进程只保留租约栅栏，不保存完成 ACK。若 Pi 在物理动作完成而 QoS 2 确认前重启，Server 应将该动作视为结果不确定，终止旧 Session，并在 Pi 回到持久配置后复核，而不能把无 ACK 推断为未执行。

待发 ACK 队列容量为 128 条，限制的是尚未得到 MQTT 完成确认的积压量，不是 Session 内已执行动作的总次数。队列满时 Pi 暂不执行新的链路排障动作，避免产生无处保存的成功终态；Server 须限制在途请求，并在无 ACK 时保持结果未确认。去重缓存也保留最近最多 128 条动作终态，满时淘汰最旧记录；淘汰后再次到达且已超过 `expires_at` 的旧请求会被拒绝，不能把缓存淘汰理解为放宽租约或动作有效期。

配置示例不增加实验字段：仍使用现有 `serial.baud` 作为持久基准、`identity.binding_file` 所在目录保存本机租约记录、`cellular.heartbeat_interval_ms` 作为周期状态发送间隔。

## 本地验证与现场边界

本地执行 `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`、`cmake --build build -j2`、`ctest --test-dir build --output-on-failure`。`test_link_lab` 覆盖严格字段、到期与租约、首次写帧、诊断位、换速、重投、重启和恢复。实体 Pi 验收须依次记录版本、串口拓扑、真实 MQTT ACK、F407 业务帧和箱体显示；需特别观察 5G 先修但 UART 仍失配，以及先修 UART 后屏幕仍呈教学断开。未取得这些现场证据前，不宣称跨端实验已通过。
