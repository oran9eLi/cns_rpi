# MQTT 注册/发现机制设计

版本：2026-07-10

状态：已确认，待实施计划

适用范围：RPi 端 MQTT 设备注册、发现与在线状态维护

## 1. 背景与目标

现有遥测 topic 为 `{topic_prefix}/{vendor_id}/telemetry`。`vendor_id` 是设备的权威全局标识，但订阅方在首次接入时并不知道有哪些 `vendor_id`，无法按具体设备建立订阅和数据库记录。

本项目新增一套固定规则的注册/发现机制，使服务器能够：

- 不预先知道 `vendor_id`，通过 MQTT 通配符发现所有设备。
- 以 `vendor_id` 为数据库主键幂等 upsert 设备元数据。
- 获取设备在线/离线状态。
- 在角色号晚于 `vendor_id` 就绪时，通过后续注册更新补全 `dcdw_label`，不依赖遥测 payload 补数据库字段。

本项目同时让当前目标 MQTT 嵌套配置结构在 C++ 配置解析中生效，但不配置上报周期、心跳周期和重连退避；后三项属于下一个“运行时参数配置化”项目。

## 2. 范围

### 2.1 本次实现

- registration topic 构造与配置。
- online/offline 注册 JSON payload。
- retained、QoS 2 的注册发布。
- MQTT Last Will 异常离线通知。
- 首次连接、自动重连和身份元数据变化时的注册发布。
- `SIGINT`/`SIGTERM` 正常退出时主动发布 offline。
- 当前 MQTT 配置从扁平结构迁移到已确定的 `connection`、`auth`、`topics` 嵌套结构。
- telemetry topic suffix 和 QoS 改由嵌套配置提供，最终 topic 字符串保持兼容。

### 2.2 本次不实现

- MQTT 命令订阅和 ACK topic；仅保证当前 topic 层级可自然扩展这两类消息。
- 运行时动态修改配置文件。
- 上报周期、MAVLink 心跳周期和 MQTT 重连退避配置化。
- 日志分级和日志文件输出。
- 服务端数据库程序；本文只定义服务端应遵守的 upsert 契约。
- MQTT TLS、证书和访问控制。
- 5G 模块、SIM 卡或蜂窝网络联调。

## 3. 方案选择

### 3.1 采用方案：设备维度同层 topic

```text
{namespace}/{vendor_id}/registration
{namespace}/{vendor_id}/telemetry
```

服务端使用以下通配符发现全部设备：

```text
{namespace}/+/registration
```

该结构以设备为第二层，registration、telemetry 以及后续 command、ack 使用同一命名风格。

### 3.2 未采用方案

- `{namespace}/registration/{vendor_id}`：可以发现设备，但注册与 telemetry 的设备维度层级不一致，会形成两套订阅规则。
- `{namespace}/registration`：所有设备共用一个 retained topic 时，后发布的设备会覆盖先发布设备的保留消息，不能形成多设备目录。

## 4. 配置契约

目标配置结构：

```json
"mqtt": {
  "connection": {
    "host": "localhost",
    "port": 1883,
    "keepalive": 60,
    "client_id": "cns-rpi"
  },
  "auth": {
    "username": "",
    "password": ""
  },
  "topics": {
    "namespace": "cns_rpi",
    "registration": {
      "suffix": "registration",
      "qos": 2
    },
    "telemetry": {
      "suffix": "telemetry",
      "qos": 0
    }
  }
}
```

字段含义：

| 字段 | 类型 | 约束 |
|---|---|---|
| `mqtt.connection.host` | string | 非空 broker 主机名或地址 |
| `mqtt.connection.port` | integer | `1..65535` |
| `mqtt.connection.keepalive` | integer | 正整数，单位为秒 |
| `mqtt.connection.client_id` | string | 非空，作为 Client ID 前缀；实际值派生为 `{前缀}-{vendor_id}` |
| `mqtt.auth.username` | string | 空字符串表示不配置用户名认证 |
| `mqtt.auth.password` | string | 用户名非空时传给 libmosquitto |
| `mqtt.topics.namespace` | string | 非空，不含 `/`、`+`、`#` |
| `mqtt.topics.registration.suffix` | string | 非空，不含 `/`、`+`、`#` |
| `mqtt.topics.registration.qos` | integer | `0..2`，产品配置使用 `2` |
| `mqtt.topics.telemetry.suffix` | string | 非空，不含 `/`、`+`、`#` |
| `mqtt.topics.telemetry.qos` | integer | `0..2`，产品配置使用 `0` |

配置文件不可读、字段缺失、类型错误或取值越界时，程序启动失败并输出明确错误，不使用静默默认值。本项目只迁移 MQTT 配置；`serial`、`logging`、`identity` 和 `cellular` 配置保持原有结构。

多台设备不能使用相同的 MQTT Client ID，否则 broker 会断开先建立的同名连接。因此配置中的 `client_id` 是产品前缀，不是最终值；对示例配置和 `vendor_id=ABC123`，实际连接使用 `cns-rpi-ABC123`。`vendor_id` 只包含设备标识允许的字符；若最终 Client ID 超出 broker 限制，配置校验阶段报错，不静默截断。

## 5. Topic 契约

新增接口：

```cpp
std::string BuildRegistrationTopic(const std::string& topic_namespace,
                                   const std::string& vendor_id,
                                   const std::string& suffix);

std::string BuildTelemetryTopic(const std::string& topic_namespace,
                                const std::string& vendor_id,
                                const std::string& suffix);
```

对示例配置和 `vendor_id=ABC123`：

```text
cns_rpi/ABC123/registration
cns_rpi/ABC123/telemetry
```

topic 构造函数只负责拼接；配置合法性由 `LoadAppConfig()` 统一校验，避免在运行循环重复验证。

## 6. 注册消息契约

### 6.1 Online 完整注册

```json
{
  "schema_version": 1,
  "vendor_id": "厂商唯一产品识别码",
  "school_name": "NNUTC",
  "dcdw_label": "DCDW-001",
  "status": "online"
}
```

### 6.2 角色号未就绪的 Online 注册

```json
{
  "schema_version": 1,
  "vendor_id": "厂商唯一产品识别码",
  "school_name": "NNUTC",
  "status": "online"
}
```

### 6.3 Offline 注册

```json
{
  "schema_version": 1,
  "vendor_id": "厂商唯一产品识别码",
  "status": "offline"
}
```

字段规则：

| 字段 | 规则 |
|---|---|
| `schema_version` | 必填，当前固定为整数 `1` |
| `vendor_id` | 必填，数据库 upsert 主键，与 topic 中的设备段一致 |
| `school_name` | online 时必填，来自 `identity.school_name` |
| `dcdw_label` | online 时可选；未就绪则省略，不发送空字符串或 `null` |
| `status` | 必填，只允许 `online` 或 `offline` |

registration payload 使用“局部 upsert”语义：字段缺失表示本次不更新该字段，不表示清空数据库旧值。因此 offline Last Will 不携带 `school_name` 或 `dcdw_label`，服务器只更新在线状态。

注册 payload 由独立模块构造，不把 JSON 字段拼装直接堆在 `main.cpp`：

```text
src/registration/registration_payload.hpp
src/registration/registration_payload.cpp
```

## 7. 客户端接口与职责

`mqtt::ConnectionOptions` 增加 Last Will 配置，Last Will 必须在 `mosquitto_connect_async()` 之前通过 `mosquitto_will_set()` 设置：

```cpp
struct WillOptions {
  std::string topic;
  std::string payload;
  int qos = 2;
  bool retain = true;
};

struct ConnectionOptions {
  std::string broker_host;
  int broker_port = 0;
  std::string client_id;
  std::string username;
  std::string password;
  int keepalive_seconds = 0;
  WillOptions will;
};
```

`MqttClient` 继续负责 libmosquitto 生命周期、连接状态和发布，不负责构造 topic、注册 JSON 或判断何时注册。

为正常退出增加一个有界确认接口：

```cpp
bool PublishAndWait(const std::string& topic,
                    const std::string& payload,
                    int qos,
                    bool retain,
                    std::chrono::milliseconds timeout);
```

该接口以 libmosquitto 的 publish completion 回调确认发送完成，最多等待调用方指定的时间。普通 telemetry 和 online 注册仍使用非阻塞 `Publish()`；只有正常退出的 offline 发布使用 `PublishAndWait()`。

## 8. 主循环状态机

### 8.1 身份与连接

1. UART 链路按现有流程启动。
2. `vendor_id` 未就绪时不创建 MQTT 客户端。
3. `vendor_id` 就绪后派生唯一 MQTT Client ID，并构造 registration topic、telemetry topic、offline payload 和 Last Will，随后创建客户端。
4. `dcdw_label` 不作为连接前置条件。

### 8.2 注册发布

主程序维护：

- 上一次观察到的 MQTT 连接状态。
- 上一次成功发布的 online 注册内容。

在以下情况发布 retained online 注册：

- MQTT 状态从未连接变为已连接，包括首次连接和自动重连。
- 当前 online 注册内容与上一次成功发布内容不同，包括 `dcdw_label` 从无到有或值发生变化。
- 上一次发布失败，因而没有更新“最后成功发布内容”。

内容未变化且连接状态没有发生上升沿时不重复发布。online 注册发布失败不阻塞 telemetry；主循环保留未注册状态并在后续迭代重试。

### 8.3 异常离线与重连

- 网络断开、进程崩溃或设备掉电时，由 broker 发布 QoS 2、retained 的 offline Last Will。
- libmosquitto 自动重连成功后，连接状态出现新的上升沿，主程序重新发布 online，覆盖 retained offline。
- QoS 重投可能产生重复注册，服务端必须按 `vendor_id` 幂等处理。

### 8.4 正常退出

- 安装 `SIGINT` 和 `SIGTERM` 处理函数。
- 信号处理函数只设置 `volatile std::sig_atomic_t` 退出标志，不调用 MQTT、日志、内存分配或其他非异步信号安全操作。
- 主循环观察到退出标志后，调用 `PublishAndWait()` 发布 retained offline，等待上限固定为 2 秒。
- 发布确认或等待超时后停止 MQTT 客户端并退出。
- 若连接已异常断开，broker 的 Last Will 负责 offline 兜底。

## 9. 服务端契约

服务端订阅：

```text
{namespace}/+/registration
```

处理规则：

1. 校验 topic 中的 `vendor_id` 与 payload 的 `vendor_id` 一致。
2. 校验 `schema_version=1` 和 `status` 枚举。
3. 以 `vendor_id` 为唯一主键执行幂等 upsert。
4. 只更新 payload 中实际存在的字段，缺失字段不清空旧值。
5. `status=offline` 时保留设备元数据，仅更新在线状态。
6. 允许 QoS 2 重连、会话恢复或应用重试产生语义重复消息。

服务器不从 telemetry payload 中提取 `dcdw_label` 补设备表。角色号就绪或变化后由 RPi 重发 registration 完成元数据同步。

## 10. 文件边界

预计修改：

| 文件 | 职责变化 |
|---|---|
| `config/config.example.json` | 使用本设计的嵌套 MQTT 配置 |
| `src/config/app_config.hpp/.cpp` | 表达并解析嵌套连接、认证、registration 和 telemetry 配置，执行取值校验 |
| `src/mqtt/topic.hpp/.cpp` | 构造 registration 和可配置 suffix 的 telemetry topic |
| `src/mqtt/mqtt_client.hpp/.cpp` | 设置 Last Will，增加有界发布确认和正常断开能力 |
| `src/registration/registration_payload.hpp/.cpp` | 构造 online/offline 注册 JSON |
| `src/main.cpp` | 注册状态机、身份变化检测和信号退出编排 |
| `CMakeLists.txt` | 编译注册 payload 模块并增加测试目标 |
| `tests/test_app_config.cpp` | 嵌套配置与非法配置测试 |
| `tests/test_mqtt_topic.cpp` | registration/telemetry topic 测试 |
| `tests/test_registration_payload.cpp` | 注册 JSON 字段契约测试 |
| `tests/test_registration_state.cpp` | 连接上升沿、身份变化、失败重试和去重状态测试 |
| `docs/V1设计文档.md` | 更新注册/发现机制和 M5/M6 topic 说明 |

注册状态判断应提取成不依赖 libmosquitto 的小型纯逻辑单元，避免只能通过 `main.cpp` 集成行为测试。

## 11. 错误处理

- Last Will 设置失败：`MqttClient::Open()` 返回 `std::nullopt`，主循环下一轮重试创建客户端。
- MQTT 连接失败：沿用 libmosquitto 自动重连；未连接期间不发布。
- online 注册发布失败：记录错误，不更新最后成功注册快照，后续迭代重试。
- telemetry 发布失败：沿用当前下个节拍重试策略，与注册状态解耦。
- 正常退出 offline 发布超时：记录错误并继续退出，不无限阻塞 systemd stop。
- 配置错误：启动失败，不带错误配置进入运行循环。

## 12. 验证方案

### 12.1 自动化测试

- `app_config`：完整嵌套配置、缺字段、字段类型错误、端口范围、keepalive、QoS 范围和 topic 段非法字符。
- `mqtt_topic`：registration 与 telemetry 的准确字符串。
- `registration_payload`：online 有角色号、online 无角色号、offline 的字段集合和值。
- 注册状态纯逻辑：首次连接、重连、角色号补全、角色号变化、内容不变、发布失败后重试。
- 既有 UART、协议、状态、JSON 和 MQTT topic 测试保持通过。
- CMake 全量构建零错误；现有编译警告策略不新增警告。

### 12.2 本地 Mosquitto 联调

不依赖 5G 模块或 SIM 卡，在本地 broker 验证：

1. 订阅 `{namespace}/+/registration`，设备首次连接后收到 retained online。
2. 新订阅者加入后立即收到 retained 注册消息。
3. `dcdw_label` 从缺失变为有效值时，同一 topic 的 retained 内容被更新。
4. `SIGTERM` 后收到主动发布的 offline。
5. 强制终止或断开网络后收到 broker 发布的 offline Last Will。
6. 自动重连后 online 覆盖 offline。
7. telemetry topic 字符串保持 `{namespace}/{vendor_id}/telemetry`，QoS 使用新配置值。

## 13. 验收标准

- 订阅方无需预知任何 `vendor_id` 即可发现所有设备。
- 每台设备拥有独立 retained registration topic，不互相覆盖。
- 每台设备使用由 `vendor_id` 派生的唯一 MQTT Client ID，不发生同名连接互踢。
- `vendor_id` 就绪即可上线，缺失 `dcdw_label` 不阻塞 MQTT。
- 角色号后续就绪或变化时，服务器只依赖 registration 即可更新数据库。
- 异常断线由 Last Will 标记 offline，正常停止由主动发布标记 offline。
- 自动重连后重新标记 online。
- 注册失败不阻塞 telemetry，且会重试。
- MQTT 嵌套示例配置能被当前程序解析，示例配置与代码不再脱节。
- 不引入 5G/SIM 依赖，不扩展到 M6 命令下行、运行时参数或日志分级。
