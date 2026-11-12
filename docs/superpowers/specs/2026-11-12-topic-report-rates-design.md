# Topic 上报周期控制

用户目标：服务器可下发指令调整所有数据上报 Topic 的周期，覆盖普通遥测和实验数据。设备端实现，对接文档同步；不修改服务器、F407，不部署现场设备。

用户此前已将 Pi→服务器接口的实现方式交由设备端确定。本次已明确确认：所有数据 Topic 可调，注册与 ACK 立即发送；GNSS 保留全部原帧，只调整组批周期。

## 方案

- 复用原有配置命令并沿用重启生效：改动较少，但中断 GNSS 采集和视频，不能满足实验中调频。
- 新建一套临时调频命令：能热生效，但增加 Topic、凭据和配置状态。
- 采用：扩展 `config/set` 的 `topic_publish_intervals_ms`，复用持久化和应答，纯调频命令热生效。与原来需要重启的参数分开下发。

## 契约

请求 Topic：`{namespace}/{device_id}/config/set`，QoS 2、retain=false。
应答 Topic：`{namespace}/{device_id}/config/ack`，QoS 2、retain=false。

```json
{
  "command_id": "report-rate-001",
  "parameters": {
    "topic_publish_intervals_ms": {
      "telemetry/snapshot/v1": 1000,
      "telemetry/realtime/v1": 200,
      "runtime/status/v1": 5000,
      "imu/sample/v1": 500,
      "gnss/raw/v1": 1000
    }
  }
}
```

字典键为本设备数据 Topic 后缀，快照与实时使用设备实际配置的后缀；其余为固定后缀。无需 namespace、device_id，不接受通配符、其他设备或未知 Topic。

值为 100～60000 的整数毫秒。只给部分 Topic 即只更新这些 Topic。IMU 另允许 0，表示恢复默认随每个完整新采样对发送。调频命令不得混入原来需要重启的四类参数。

应答保留 command_id、status、restart_required，并在成功/重复调频应答带当前完整 `topic_publish_intervals_ms`。成功为 applied、restart_required=false；重复 command_id 为 already_applied，不重写、不覆盖后来设置。失败为 rejected，写失败不改变内存运行值。提交不确定沿用已有明确错误与重启核对语义。

## 各 Topic 语义

| Topic | 调频作用 | 默认 |
| --- | --- | --- |
| telemetry/snapshot/v1 | 周期重新生成最新快照 | 1000ms |
| telemetry/realtime/v1 | 周期重新生成最新实时帧，保持原序号 | 100ms |
| runtime/status/v1 | 内容未变时的周期重发；内容变化和 MQTT 重连仍立即发送 | 10000ms |
| imu/sample/v1 | 新完整采样对发送的最小间隔；不重发旧采样，不提升 F407 采样率 | 0，随新采样发送 |
| gnss/raw/v1 | 组批等待周期；达到 32 帧/16384B 或采集结束仍提前 flush，不丢原文 | 200ms |

RTK 在快照/实时内，随对应 Topic 周期变化。视频的命令应答仍按事件发送，本功能不改变 SRT/WebRTC 帧率、编码参数或媒体服务接口。注册上下线、遗嘱与所有 ACK 不调频。

配置落盘后更新内存调度，无需关闭 MQTT、串口、采集或视频；纯调频可在实验进行中执行。保持原身份门禁、QoS、retain、原始数据大小/数量限额和已有失败语义。

快照与实时仍保存到原 telemetry_publish 配置。新增可选 reporting，包含 runtime_status_interval_ms、imu_sample_interval_ms、gnss_batch_interval_ms；缺省保持既有行为。保留未知现场配置字段，不修改生成的 MAVLink 文件。

## 验证范围

测试直接调用现有解析、配置处理、发布调度和 GNSS 实现；进程集成使用实际 cns_rpi、PTY 和隔离 broker，验证调频 ACK、实时节奏、实验进行中生效、重启后保留。软件验证与现场设备或服务器验收分别说明，验证记录不进入对外接口正文。
