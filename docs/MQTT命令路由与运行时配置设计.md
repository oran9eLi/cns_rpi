# MQTT 命令路由与运行时配置设计

版本：2026-07-10

状态：RPi 设备端已实施，服务器端待在 `cns_server` 仓库实施

适用范围：CNS 设备、硬件部上位机、管控中心、`cns_server` 与目标 RPi 之间的配置命令流转，以及 RPi 运行参数持久化和 systemd 重启生效流程。

## 1. 背景与目标

当前 RPi 程序仍有四个运行参数写死在代码中：

- `src/main.cpp` 的 `kTelemetryPublishInterval`：遥测上报间隔。
- `src/main.cpp` 的 `kHeartbeatInterval`：RPi 向 STM32 发送 MAVLink HEARTBEAT 的间隔。
- `src/mqtt/mqtt_client.cpp` 的 `reconnect_delay`：MQTT 重连初始等待。
- `src/mqtt/mqtt_client.cpp` 的 `reconnect_delay_max`：MQTT 重连最大等待。

本设计的目标不只是把常量搬进 `config.json`，还要建立一条可审计、可幂等、可跨设备路由的配置修改链路：

```text
命令来源
  -> cns_server 统一入口
  -> 数据库寻址和权限校验
  -> 目标 RPi
  -> 持久化 config.json
  -> ACK
  -> 主动退出
  -> systemd 重新拉起
  -> 新配置生效
```

## 2. 系统边界

### 2.1 命令来源

可能的命令来源包括：

- CNS 平级设备。
- 硬件部服务器上运行的上位机进程。
- 经 MQTT 桥接接入的管控中心。

每个独立命令来源拥有一个固定、全局唯一的 `source_id`：

| 来源 | `source_id` 规则 |
|---|---|
| CNS 设备 | 使用本设备 `vendor_id` |
| 硬件部上位机 | 由服务器管理员分配，例如 `hardware-console` |
| 管控中心 | 由服务器管理员分配，例如 `main-control` |

`source_id` 不能使用 PID、临时 IP、每次启动新生成的 UUID 或临时 MQTT Client ID。

### 2.2 服务器职责

`cns_server` 是所有外部配置命令的唯一入口和唯一设备命令发布者，负责：

- 登记和识别命令来源。
- 按来源执行权限检查。
- 把学校名和内部编号解析为目标 `vendor_id`。
- 生成服务器内部 `command_id`。
- 持久化命令状态和幂等记录。
- 向目标设备转发规范化命令。
- 接收目标设备 ACK，并把结果路由回原命令来源。

即使命令来源已经知道目标 `vendor_id`，也必须先经过 `cns_server`，不能直接向设备命令 topic 发布。

### 2.3 RPi 职责

本轮 RPi 实现范围：

- 订阅本设备的配置命令 topic。
- 解析和校验四个白名单参数。
- 幂等判断。
- 持久化完整 `config.json`。
- 发布 ACK。
- 配置成功后主动退出，由 systemd 重新拉起。
- 预留本设备向服务器提交同校设备配置请求的发布接口。

本轮不提供设备端本地 UI、按键、Qt 页面或其他实际命令触发入口。

### 2.4 本轮不实现

- `cns_server` 的实际 DDL、MQTT 路由进程和管理 API；这些内容在服务器仓库单独实施。
- M6 四路电机占空比命令链路。
- 日志分级和 `logging.file` 行为。
- TLS、MQTT ACL 和来源认证的完整生产配置。
- Qt、本地 HTTP API 或设备端命令 UI。
- M7 的完整 OverlayFS、journald 限额和 systemd 看门狗。

## 3. 总体命令流转

```text
CNS设备 / 硬件部上位机 / 管控中心
        |
        | {namespace}/sources/{source_id}/config/request
        v
cns_server
  1. 从 topic 提取 source_id
  2. 校验来源是否登记、启用
  3. 按 (source_id, request_id) 幂等处理
  4. 校验目标寻址和学校权限
  5. 查询数据库得到 target_vendor_id
  6. 生成 command_id 并记录映射
        |
        | {namespace}/{target_vendor_id}/config/set
        v
目标 RPi
  1. 校验 topic、command_id 和参数
  2. 检查 applied_command_ids
  3. 生成完整候选配置
  4. 持久化 config.json
  5. 发布执行 ACK
  6. 主动退出
        |
        | {namespace}/{target_vendor_id}/config/ack
        v
cns_server
  1. 按 command_id 查原始来源
  2. 更新命令状态
  3. 返回来源 request_id 和执行结果
        |
        | {namespace}/sources/{source_id}/config/ack
        v
原命令来源
```

## 4. MQTT Topic 契约

| Topic | 方向 | QoS | Retain | 用途 |
|---|---|---:|---|---|
| `{namespace}/sources/{source_id}/config/request` | 来源 → 服务器 | 2 | false | 向服务器提交配置请求 |
| `{namespace}/sources/{source_id}/config/ack` | 服务器 → 来源 | 2 | false | 返回路由或执行结果 |
| `{namespace}/{vendor_id}/config/set` | 服务器 → 目标设备 | 2 | false | 下发规范化配置命令 |
| `{namespace}/{vendor_id}/config/ack` | 目标设备 → 服务器 | 2 | false | 返回目标设备执行结果 |

所有配置命令必须使用 `retain=false`，防止设备以后上线时执行陈旧命令。离线设备的待处理命令由服务器数据库维护，服务器在设备重新 online 后决定是否重发，不能依赖 retained 命令。

`source_id` 放在 topic 中，而不是让发送方在 payload 中重复声明。服务器从 topic 取得来源身份，避免 topic 与 payload 来源不一致，并为后续 MQTT ACL 提供清晰边界。

## 5. 来源请求协议

### 5.1 请求号

命令来源为每次逻辑请求生成 `request_id`。同一请求因超时而重发时必须复用原 `request_id`，不能生成新值。

服务器以以下二元组作为来源请求幂等键：

```text
(source_id, request_id)
```

`request_id` 推荐使用 UUID。具体 UUID 版本不影响协议，服务器只把它当作来源作用域内的唯一不透明字符串。

### 5.2 设备来源请求

设备来源只能按同校内部编号寻址，payload 不允许填写学校名或目标 `vendor_id`：

```json
{
  "request_id": "来源生成的唯一请求号",
  "target": {
    "dcdw_label": "DCDW-002"
  },
  "parameters": {
    "telemetry_publish_interval_ms": 2000
  }
}
```

设备发布到：

```text
{namespace}/sources/{本设备vendor_id}/config/request
```

### 5.3 上位机或管控中心请求

按学校和内部编号寻址：

```json
{
  "request_id": "来源生成的唯一请求号",
  "target": {
    "school_name": "SEU",
    "dcdw_label": "DCDW-002"
  },
  "parameters": {
    "heartbeat_interval_ms": 2000
  }
}
```

已知全局标识时也可按 `vendor_id` 寻址：

```json
{
  "request_id": "来源生成的唯一请求号",
  "target": {
    "vendor_id": "DCDWCNS1ABCDEFGHIJKL"
  },
  "parameters": {
    "mqtt_reconnect_delay_max_s": 60
  }
}
```

一条请求只能采用一种目标寻址形式。混合填写、字段缺失或目标不唯一时，服务器直接拒绝。

## 6. 同校限制与来源权限

### 6.1 设备来源

设备来源必须受到双重限制：

1. RPi 端预留的请求发布接口只接受 `target_dcdw_label`，不提供 `school_name` 或目标 `vendor_id` 参数。
2. `cns_server` 根据 source topic 中的设备 `vendor_id` 查询源设备所属学校，只在相同 `school_id` 内查找目标 `dcdw_label`。

服务器查询语义：

```sql
SELECT vendor_id
FROM devices
WHERE school_id = :source_school_id
  AND dcdw_label = :target_dcdw_label;
```

即使恶意设备手工构造包含其他学校的 payload，服务器也必须拒绝。

### 6.2 上位机和管控中心

上位机和管控中心按服务器登记的学校权限访问目标。即使请求直接使用目标 `vendor_id`，服务器仍需检查目标所属学校是否在该来源的权限范围内。

来源类型属于服务器数据库元数据，不由每条命令 payload 声明，避免错误或伪造 `source_type`。

## 7. 服务器数据库扩展

`cns_server` 当前 README 已定义 `schools` 和 `devices`。命令路由还需要以下表。

### 7.1 command_sources

| 字段 | 说明 |
|---|---|
| `source_id` | 全局唯一固定来源标识，主键 |
| `source_kind` | device / host_app / control_center，仅服务器内部使用 |
| `device_vendor_id` | 设备来源关联的 `vendor_id`，其他来源为空 |
| `enabled` | 是否允许发送命令 |
| `created_at` | 登记时间 |

### 7.2 source_school_permissions

| 字段 | 说明 |
|---|---|
| `source_id` | 命令来源 |
| `school_id` | 允许访问的学校 |

设备来源不使用该表扩展权限，始终强制同校。该表只用于上位机和管控中心。

### 7.3 config_commands

| 字段 | 说明 |
|---|---|
| `command_id` | 服务器生成的唯一命令号，主键 |
| `source_id` | 原始命令来源 |
| `request_id` | 来源生成的请求号 |
| `target_vendor_id` | 解析后的目标设备 |
| `parameters` | 规范化参数 JSON |
| `status` | pending / dispatched / applied / already_applied / rejected / timeout |
| `error_code` | 失败原因 |
| `created_at` | 创建时间 |
| `completed_at` | 完成时间 |

唯一约束：

```text
UNIQUE(source_id, request_id)
```

重复规则：

- 相同 `(source_id, request_id)` 且目标和参数相同：返回已有状态，不重复下发。
- 相同 `(source_id, request_id)` 但目标或参数不同：拒绝并返回 `idempotency_conflict`。

## 8. 服务器转发协议

服务器完成来源校验、目标解析、权限检查和来源幂等后，生成全局唯一 `command_id`，只向目标设备发送规范化参数：

```json
{
  "command_id": "服务器生成的唯一命令号",
  "parameters": {
    "telemetry_publish_interval_ms": 2000,
    "mqtt_reconnect_delay_max_s": 60
  }
}
```

目标设备不需要知道原始来源、学校、内部编号或 `request_id`。

## 9. RPi 配置结构

在现有 `config.json` 增加 `runtime` 节和 MQTT 重连节：

```json
{
  "runtime": {
    "telemetry_publish_interval_ms": 1000,
    "heartbeat_interval_ms": 1000,
    "applied_command_ids": []
  },
  "mqtt": {
    "connection": {
      "host": "localhost",
      "port": 1883,
      "keepalive": 60,
      "client_id": "cns-rpi",
      "reconnect": {
        "delay_s": 1,
        "delay_max_s": 30
      }
    }
  }
}
```

参数约束：

| 字段 | 允许范围 | 说明 |
|---|---:|---|
| `runtime.telemetry_publish_interval_ms` | 100～60000 ms | 遥测上报间隔 |
| `runtime.heartbeat_interval_ms` | 100～60000 ms | RPi 向 STM32 发送 HEARTBEAT 的间隔 |
| `mqtt.connection.reconnect.delay_s` | 1～3600 s | MQTT 重连初始等待 |
| `mqtt.connection.reconnect.delay_max_s` | 1～3600 s | MQTT 重连最大等待 |
| `runtime.applied_command_ids` | 最多 32 个合法命令号 | 防止设备重启后重复执行近期服务器命令 |

附加约束：

- `delay_s <= delay_max_s`。
- 指数退避保持开启，不做成配置项。
- 命令允许部分更新，未出现的字段保持原值。
- 任一字段非法则整条命令拒绝，不允许部分写入。
- `applied_command_ids` 由程序内部维护，远程命令不能直接修改；新命令成功持久化时追加，超过 32 条删除最旧记录。
- 远程命令不能修改串口、MQTT 地址、认证、topic、学校名称、蜂窝 APN 或配置写入模式。

## 10. 目标设备处理流程

目标设备订阅：

```text
{namespace}/{本设备vendor_id}/config/set
```

执行顺序：

1. 从 MQTT topic 校验目标 `vendor_id` 属于本设备。
2. 解析 JSON，校验 `command_id` 和 `parameters`。
3. 读取当前 `runtime.applied_command_ids`。
4. 若命令已执行，返回 `already_applied`，不写配置、不退出。
5. 读取原始完整 JSON，生成候选配置，只修改四个白名单参数。
6. 把当前 `command_id` 追加到候选配置的 `applied_command_ids`，并把列表裁剪为最近 32 条。
7. 调用配置持久化后端写盘。
8. 持久化成功后发布 `applied` ACK，最多等待 2 秒。
9. 无论 ACK 成功还是超时，持久化成功后都主动正常退出。
10. systemd 重新拉起程序，新进程读取新配置。

若持久化失败，返回 `rejected`，不退出，原配置继续运行。

## 11. 目标设备 ACK

成功：

```json
{
  "command_id": "服务器命令号",
  "status": "applied",
  "restart_required": true
}
```

重复命令：

```json
{
  "command_id": "服务器命令号",
  "status": "already_applied",
  "restart_required": false
}
```

失败：

```json
{
  "command_id": "服务器命令号",
  "status": "rejected",
  "error_code": "invalid_parameter",
  "message": "telemetry_publish_interval_ms超出允许范围",
  "restart_required": false
}
```

错误码：

| 错误码 | 含义 |
|---|---|
| `invalid_json` | payload 不是合法 JSON |
| `invalid_command_id` | 命令号缺失或非法 |
| `unknown_parameter` | 出现白名单外参数 |
| `invalid_parameter_type` | 参数类型错误 |
| `invalid_parameter` | 参数越界或组合关系非法 |
| `config_read_failed` | 读取原始配置失败 |
| `config_write_disabled` | 未启用配置写入后端 |
| `config_write_failed` | 配置持久化失败 |

## 12. 配置持久化

### 12.1 原子写入要求

配置更新必须：

1. 在目标配置同一文件系统中创建临时文件。
2. 写入完整候选 JSON。
3. 对临时文件执行 `fsync`。
4. 使用 `rename` 原子替换目标文件。
5. 对配置目录执行 `fsync`。

任何一步失败都不得破坏原配置文件。

### 12.2 写入后端选择

程序不能自动猜测开发环境或生产环境。配置写入后端通过可信启动参数明确指定，不能放入可远程修改的 `config.json`。

支持三种模式：

| 模式 | 行为 |
|---|---|
| `disabled` | 默认值；拒绝配置修改命令 |
| `direct` | 普通可写文件系统，C++ 直接执行原子写入 |
| `helper` | 产品 OverlayFS 环境，通过受限辅助脚本持久化 |

开发环境：

```bash
./build/cns_rpi config/config.json --config-writer=direct
```

生产环境：

```ini
ExecStart=/home/dcdw/cns_rpi/build/cns_rpi \
  /home/dcdw/cns_rpi/config/config.json \
  --config-writer=helper \
  --config-helper=/usr/local/libexec/cns-rpi-apply-config
```

`helper` 路径来自 systemd 启动参数。MQTT 命令不能修改写入模式或脚本路径。

### 12.3 OverlayFS 辅助脚本

产品辅助脚本按最小权限原则执行：

- 只接受固定运行目录中的候选配置。
- 只允许写入固定的 `config.json`。
- 校验候选文件是合法 JSON。
- 临时切换所需写层或挂载状态。
- 执行原子替换和 `sync`。
- 恢复只读状态。
- 失败时返回非零，不让 C++ 进程误判成功。

sudoers 只允许执行该固定脚本，不给业务进程完整 shell 或任意 `mount` 权限。

## 13. systemd 重启闭环

本项目提供最小 `cns-rpi.service`：

```ini
[Service]
Restart=always
RestartSec=2
```

配置应用成功后程序正常退出。systemd 在 2 秒后拉起新进程，新进程读取新配置。

systemd 收到明确的 `stop` 操作时不会因 `Restart=always` 再拉起，因此不影响正常运维停机。

M7 后续在该最小服务基础上增加看门狗、journald 限额、OverlayFS 系统配置和完整部署脚本。

## 14. RPi 软件组件

```text
src/mqtt/
└─ mqtt_client
   ├─ 订阅 topic
   ├─ 自动重连后重新订阅
   └─ 回调只复制消息到线程安全队列

src/config_command/
├─ command_parser       解析和校验命令
├─ config_updater       对原始 JSON 做白名单部分更新
├─ config_store         disabled/direct/helper 持久化后端
└─ command_processor    去重、持久化、ACK 和退出决策

src/main.cpp
├─ 从 MQTT 队列取命令
├─ 调用 command_processor
├─ 发布 ACK
└─ 持久化成功后退出
```

### 14.1 MQTT 线程边界

- libmosquitto 回调运行在后台网络线程。
- 回调中不解析大 JSON、不写文件、不执行脚本、不退出进程。
- 回调只复制 topic 和 payload 到线程安全队列。
- 主循环负责解析、持久化、ACK 和退出。
- 自动重连成功后重新订阅本设备 `config/set`。
- 队列容量固定为 64；队列满时记录错误并丢弃新消息，服务器因未收到 ACK 可超时重试。

### 14.2 设备来源请求预留接口

```cpp
bool PublishConfigRequest(
    const std::string& request_id,
    const std::string& target_dcdw_label,
    const ConfigParameterPatch& parameters);
```

接口固定发布到：

```text
{namespace}/sources/{本设备vendor_id}/config/request
```

接口不接受 `source_id`，内部固定使用本设备 `vendor_id`；同时不提供目标 `school_name` 或 `vendor_id` 参数，从调用边界限制设备不能伪装其他来源，也只能按同校内部编号请求。服务器仍必须执行权威来源和同校校验。

本轮只预留接口，不增加调用入口。

## 15. 服务器结果回程

目标设备 ACK 到达后，服务器按 `command_id` 查询 `config_commands`，更新执行状态，并向原来源返回：

```json
{
  "request_id": "来源请求号",
  "command_id": "服务器命令号",
  "status": "applied",
  "target": {
    "vendor_id": "目标设备vendor_id",
    "school_name": "SEU",
    "dcdw_label": "DCDW-002"
  }
}
```

来源只需要按自己的 `request_id` 匹配结果。目标设备只需要按服务器 `command_id` 幂等执行，两者职责分离。

## 16. 验证方案

### 16.1 自动化测试

- 四个配置字段的合法值、边界值和非法值。
- `delay_s <= delay_max_s` 组合约束。
- 部分更新不改变其他 JSON 字段。
- 未知参数导致整条命令拒绝。
- `applied_command_ids` 只能由内部写入，最多保留最近 32 条。
- 写盘失败不修改原文件。
- 临时文件、`fsync`、原子替换和目录 `fsync`。
- 重复 `command_id` 不写盘、不退出。
- MQTT 自动重连后重新订阅。
- MQTT 回调只入队，由主线程处理。
- 队列容量和溢出行为。
- ACK 各状态 payload。
- 三种配置写入模式。
- 设备请求发布接口不能表达跨校目标。
- 原有测试保持通过。

### 16.2 集成验证

- 本地 Mosquitto 模拟服务器转发命令。
- 成功更新后发布 ACK、进程退出、systemd 拉起。
- 新进程读取新间隔并表现出新的节拍。
- 相同命令重发返回 `already_applied`，不再次退出。
- 非法命令返回 `rejected`，程序继续运行。
- 开发环境 `direct` 原子写入。
- 产品环境 `helper` 脚本持久化。
- OverlayFS 环境断电重启后配置仍保留。
- Raspberry Pi 5 ARM64 完整构建与测试。

## 17. 验收标准

- 四个写死参数全部由启动配置提供。
- 所有外部配置命令只经 `cns_server` 路由。
- 设备来源只能请求修改同校内部编号设备。
- 服务器再次强制同校限制，设备端限制不是唯一安全边界。
- 来源请求和目标执行分别具有稳定幂等键。
- 目标设备不接收来源权限或路由信息，只接收规范化参数。
- 配置更新是全有或全无，失败时保留原配置。
- 配置成功持久化后 ACK，并由 systemd 重启生效。
- ACK 丢失不会导致相同命令重复写配置和重复重启。
- 写入模式由可信启动参数选择，默认 `disabled`。
- 设计与 `cns_server` 的 `schools`、`devices` 数据库模型一致。

## 18. 设备端验证记录

验证日期：2026-07-13。

- 开发机重新配置并完整构建成功，`ctest --test-dir build --output-on-failure` 共 17 项测试全部通过。
- 开发机临时启动 Mosquitto 2.0.22，并设置 `CNS_TEST_MQTT_BROKER_PORT=18884` 运行真实 broker 集成测试；客户端实际完成连接、订阅、20 条普通 QoS 1 发布与一条 `PublishAndWait` 交错、回调入队和 PUBACK，`test_mqtt_client` 共 3 个用例、285 个断言全部通过。
- Raspberry Pi 5 ARM64（`aarch64`、GCC 14.2.0、libmosquitto 2.0.21）从当前功能分支独立配置和完整构建成功，17 项测试全部通过。
- `direct` 原子替换、写入失败保留原文件、`disabled` 拒绝写入、helper 成功/失败、重复命令不写盘不退出、非法命令 rejected、成功命令 applied 并要求退出，均由自动化测试覆盖。
- 本机缺少 `socat` 和 `pymavlink`，本轮没有伪造 UART 身份帧运行完整 `cns_rpi` 进程，因此“真实命令 topic → 完整进程持久化 → ACK → 退出”仍需在有 STM32 身份帧或专用 PTY 测试夹具的环境补做。
- 产品 helper、systemd 实际重新拉起和 OverlayFS 断电持久化尚未现场执行，按设计留到 M7 系统化部署验收；本轮只验证最小 service 契约与 helper 调用边界。
