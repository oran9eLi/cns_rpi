# Topic 上报周期控制接口

## 1. 接口

服务器通过 MQTT 修改同一设备的数据上报周期。设置保存成功后直接生效，允许在实验运行中修改；设备重启后继续使用保存值。已有数据格式、QoS、retain 和来源有效性规则保持。

默认 `namespace=cns_rpi`，`device_id` 为 F407 实验箱 SN。

| 方向 | Topic | QoS | retain | 用途 |
| --- | --- | ---: | --- | --- |
| 服务器 → Pi | `{namespace}/{device_id}/config/set` | 2 | false | 设置一个或多个数据 Topic 的周期 |
| Pi → 服务器 | `{namespace}/{device_id}/config/ack` | 2 | false | 返回保存结果及当前全部周期 |

| 数据 Topic 后缀 | 默认周期 | 调整含义 |
| --- | --- | --- |
| `telemetry/snapshot/v1` | 1000ms | 完整遥测，包含 RTK 基站/移动站观测、GNSS 定位、IMU 解析值与诊断 |
| `telemetry/realtime/v1` | 100ms | 实时遥测，包含相应实验的动态数据 |
| `runtime/status/v1` | 10000ms | 内容未变化时的状态重发周期；连接建立、重连和内容变化仍立即发送 |
| `imu/sample/v1` | 0 | 0 表示每个新完整采样对至多发送一次；正数为新采样对最小发送间隔，中间采样对不上传 |
| `gnss/raw/v1` | 200ms | 采集期间的组批等待周期，保留全部原帧；满 32 帧、达到 16KiB 消息边界或采集结束时提前成批发送 |

普通设置值为 **100～60000 的整数毫秒**，频率约为 `1000 / 周期毫秒` Hz。`imu/sample/v1` 额外允许 0，用于恢复随新配对发送。0 不表示停止；其他 Topic 不接受 0。

键使用表中的设备相对后缀；快照/实时后缀若由部署配置改名，使用设备实际后缀，且不能与其他数据、注册、命令或 ACK Topic 重名。不得填写完整 Topic、通配符或其他设备编号。仅支持表中数据 Topic；注册上下线、配置/控制/实验 ACK 都立即发送，不参与调频。

提高上报频率不会提高传感器采样率；IMU 新完整采样对仍受来源最多 5Hz 的限制，不用旧采样对补足频率。GNSS 设置控制组批等待，不是严格发送频率上限，也不改变 F407 原帧输出速率。功能未启用、没有有效来源或未开始 GNSS 采集时，设置周期不会自动启动采集或打开遥测通道。

RTK 数据沿用快照与实时 Topic，不另建 RTK 周期。视频操作通过实验 ACK 返回，视频本身走 SRT/WebRTC；视频帧率和码率使用 `video_test`、`video_start` 的视频参数设置。

## 2. 数据格式

### 2.1 设置请求

示例发送到 `cns_rpi/DCDWCNS1S2MEAG1VTA0C/config/set`：

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

| 字段 | 类型与含义 |
| --- | --- |
| `command_id` | 1～128 字节的非空字符串；每次新修改使用新 ID，原请求超时重试时保持原 ID 和内容 |
| `parameters` | 对象；纯调频命令只包含 `topic_publish_intervals_ms` |
| `topic_publish_intervals_ms` | 非空对象，Topic 后缀 → 整数毫秒。可以只提交需要修改的项，其余保持原值 |

一次请求中的全部项一起处理。未知 Topic、错误类型、超出范围或混入其他配置参数时整条拒绝，不部分应用。旧的心跳、重连等待等需要重启的配置参数单独下发。

### 2.2 成功应答

示例来自 `cns_rpi/DCDWCNS1S2MEAG1VTA0C/config/ack`：

```json
{
  "command_id": "report-rate-001",
  "status": "applied",
  "restart_required": false,
  "topic_publish_intervals_ms": {
    "telemetry/snapshot/v1": 1000,
    "telemetry/realtime/v1": 200,
    "runtime/status/v1": 5000,
    "imu/sample/v1": 500,
    "gnss/raw/v1": 1000
  }
}
```

`applied` 表示持久保存成功并更新运行周期；`topic_publish_intervals_ms` 返回当前全部五项。调频不重启进程，也不取消正在运行的实验。修改时保持原发送时刻和遥测帧序号，后续按新周期判断；主循环、数据来源及网络会造成实际接收间隔偏差。

最近 32 条成功配置命令 ID 持久去重。重复请求返回 `status="already_applied"`、`restart_required=false` 和**当前**全部周期，不重新写入；若其后执行过其他修改，重复旧请求不会恢复旧值。修改新设置应使用新的 ID。该接口没有独立查询动作；服务器保存成功应答作为已知配置。

### 2.3 拒绝应答

```json
{
  "command_id": "report-rate-002",
  "status": "rejected",
  "error_code": "invalid_parameter",
  "message": "周期必须为100至60000毫秒",
  "restart_required": false
}
```

| `error_code` | 含义与处理 |
| --- | --- |
| `invalid_json` / `invalid_command_id` | JSON 或命令 ID 错误，修正后用新 ID 下发 |
| `unknown_parameter` | Topic 或字段不受支持 |
| `invalid_parameter_type` / `invalid_parameter` | 周期类型、范围、空对象或混合参数错误 |
| `config_read_failed` | 配置无法读取、结构不完整，或磁盘 Topic 后缀与运行配置不一致；运行周期保持原值，由维护者确认并加载正确配置 |
| `config_write_failed` / `config_write_disabled` | 配置保存失败或未启用写入；运行周期保持原值 |
| `config_write_uncertain` | 保存结果不确定，`restart_required=true`，设备按已有恢复流程重启；服务器等待设备恢复后重新确认结果 |

`message` 用于显示错误原因，不作为机器判断依据。没有收到 ACK 时不推断成功，使用原 ID 和原内容重试。

## 3. 建议实验流程

1. 选择实验箱，订阅 `config/ack` 与所需数据 Topic，记录当前收到数据的节奏。
2. 下发需要修改的周期，等待相同 `command_id` 的成功应答，再更新页面中的周期显示。
3. RTK 或 IMU 曲线实验调整实时/快照周期；按真实样本时间和采样 key 绘图，不按 MQTT 消息数推算采样率。
4. GNSS 先按原接口开始采集，在采集中调整 `gnss/raw/v1` 的组批周期；按 `record_seq` 保存全部原帧，观察批大小与间隔变化。满批和结束的小批仍须接收。
5. 实验结束后如需恢复通用周期，发送一条新 ID 的配置命令；IMU 采样对设为 0 恢复随新样本发送。周期设置不会随会话结束自动恢复。
