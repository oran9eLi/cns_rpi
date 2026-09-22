# 阶段一 Pi 控制链自检实现与验证

日期：2026-09-22

状态：Pi 侧已实现并完成本地自动测试；未部署实体树莓派，未完成 Server/Web 真实闭环验收。协议以[冻结跨端契约](2026-09-22-阶段一Pi控制链自检跨端契约.md)为准，本文只记录本仓库的实现和验证方式。

## Pi 侧实现边界

- 已持久化绑定的主控箱才在现有 MQTT 客户端订阅 `{namespace}/{device_id}/experiment/set`，QoS 2；重连沿用客户端原有订阅恢复路径。PX4、未绑定设备不认领该 Topic。
- 仅接受严格 schema v1 的 `inspect_pi_link`。校验目标与绑定、UUID、租约版本、UTC 毫秒有效期、固定字段和长度；结构无法安全关联命令时只记录中文诊断，不猜测 ACK。
- 只读采样使用当前进程的运行三态、MQTT 连接状态及现有串口对象。`identity_status=cached` 和 `business_status=offline` 不阻断本项检查；`conflict|unbound` 拒绝。`serial_port_open=true` 只表示主线程持有已打开的串口端点，不能证明 UART6 参数匹配或 F407 已响应。主线程尚未接管端点时，后台自动发现可能持有候选串口，因此报告 `unknown`，不猜测为 `false`。
- 向 `{namespace}/{device_id}/experiment/ack` 发布 `completed|rejected` 终态，QoS 2、非 retained。发布调用失败时有界保留原 ACK，连接恢复后重发原文，不重新采样。异步发布调用被 MQTT 库接受但进程立即退出的极端窗口不保证终态已到达 Broker，应由 Server 按超时或待核验收敛。
- 相同 `action_id` 且内容一致的重投复用旧终态、旧观测时间和事实，仅替换本次合法 `command_id`。内容冲突拒绝。进程内最多缓存 128 个动作，每项至少保留 300 秒；Pi 进程重启后不保证保留旧采样。这个限制只适用于本轮只读动作，不能直接套用到将来的改参动作。
- 本轮没有新增 `config.json` 配置项。Topic 后缀和 QoS 是冻结契约的一部分；命名空间沿用现有配置。

## 本地验证

在隔离工作树中执行：

```sh
cmake -S . -B /tmp/cns-rpi-pi-inspect-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/cns-rpi-pi-inspect-build -j2
ctest --test-dir /tmp/cns-rpi-pi-inspect-build --output-on-failure
```

`pi_link_inspection` 覆盖成功、缓存身份与业务离线、身份冲突和无绑定、未知串口端点、内容重投、冲突、过期、错误 Topic/目标/字段、缓存容量及 ACK 待发队列。`mqtt_topic` 覆盖固定 Topic；现有测试覆盖配置、飞控命令、注册和遥测回归。

本地 Broker 重连检查为可选测试，不连接生产 Broker。先在本机临时端口启动 Mosquitto，运行下列测试；测试打印首次收发提示后停止 Broker，打印断线提示后在同一端口重新启动 Broker，检查重连后的消息仍被收到。

```sh
mosquitto -p 45883
CNS_TEST_MQTT_RECONNECT_PORT=45883 /tmp/cns-rpi-pi-inspect-build/test_mqtt_client \
  --test-case='Broker重启后自动恢复实验Topic的QoS2订阅' --no-skip
```

没有设置 `CNS_TEST_MQTT_RECONNECT_PORT` 时，该用例跳过外部 Broker 操作，不影响离线单元测试。

## ACK 形态与现场待验

成功形态示例：

```json
{"schema_version":1,"command_id":"7d15e276-c7e1-4f52-aaee-7314a6417667","session_id":"c4191037-9e2a-480c-a995-2174e4672563","action_id":"f886f45b-01f8-4a54-b2b6-a65b07477aa1","operation":"inspect_pi_link","status":"completed","observed_at":"2026-09-22T09:00:02.123Z","fact":{"control_status":"online","identity_status":"cached","business_status":"offline","serial_port_open":"unknown"}}
```

拒绝形态示例（身份不可用）：

```json
{"schema_version":1,"command_id":"7d15e276-c7e1-4f52-aaee-7314a6417667","session_id":"c4191037-9e2a-480c-a995-2174e4672563","action_id":"f886f45b-01f8-4a54-b2b6-a65b07477aa1","operation":"inspect_pi_link","status":"rejected","observed_at":"2026-09-22T09:00:02.123Z","error_code":"identity_unavailable"}
```

尚未验证：实体 Pi 上已绑定主控箱的 MQTT 订阅与 ACK；真实 F407 业务静默但 Pi 控制在线时的 `completed`；串口拔插瞬间的端点事实；实际 Broker 故障后的待发 ACK；Server 接收、落库和 Web 展示。真机验收需在获得部署授权后记录 Pi 精确提交、Server 版本、原始 MQTT 请求/ACK 与服务端入库结果；仅本地测试不能宣称跨端闭环通过。
