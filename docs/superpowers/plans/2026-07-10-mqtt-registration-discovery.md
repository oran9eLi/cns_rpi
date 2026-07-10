# MQTT 注册与发现机制实施计划

> **执行要求：** 必须使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans`，逐任务实施本计划。各步骤使用复选框（`- [ ]`）跟踪。

**目标：** 新增 MQTT 保留注册、通配符发现、在线/离线状态、元数据更新和正常退出离线发布，同时把现有 MQTT 配置迁移到已确认的嵌套结构。

**架构：** 配置、topic、JSON 和注册状态判断等纯逻辑放入 `cns_rpi_core`，无需 broker 即可单元测试。`MqttClient` 继续作为 libmosquitto 边界，增加 Last Will 和有界发布确认。`main.cpp` 负责流程编排：等待 `vendor_id`、派生唯一 Client ID、在连接上升沿或元数据变化时发布 online，并在信号触发的正常退出过程中发布 offline。

**技术栈：** C++23、CMake、doctest、nlohmann/json、libmosquitto、POSIX 信号、本地 Mosquitto 集成验证。

## 全局约束

- topic 结构使用 `{namespace}/{vendor_id}/{suffix}`；发现订阅使用 `{namespace}/+/registration`。
- `vendor_id` 是数据库主键和 MQTT Client ID 后缀。
- registration 使用 QoS 2 和 retained；telemetry 按当前 M5 行为继续使用 QoS 0 和 retained。
- 缺少 `dcdw_label` 不得阻塞连接；JSON 中应省略该字段，不能输出 `null` 或空字符串。
- 注册 payload 使用局部 upsert 语义；offline payload 不得清空已有元数据。
- 信号处理函数只能设置 `volatile std::sig_atomic_t`。
- 正常 offline 最多等待 2 秒；失败不得无限阻塞 UART 处理。
- 不实现 M6、运行时间隔配置、日志分级、TLS、服务端数据库代码或 5G/SIM 行为。
- 保留用户尚未提交的 `config/config.example.json` 内容，并让实现与其一致。

---

## 文件职责

- `src/config/app_config.hpp/.cpp`：MQTT 嵌套结构和校验。
- `src/mqtt/topic.hpp/.cpp`：registration 和 telemetry topic 构造。
- `src/registration/registration_payload.hpp/.cpp`：online/offline JSON 构造。
- `src/registration/registration_state.hpp/.cpp`：纯逻辑发布决策状态机。
- `src/mqtt/mqtt_client.hpp/.cpp`：Last Will 和有界发布确认。
- `src/main.cpp`：身份/连接编排和正常退出。
- `tests/test_app_config.cpp`：嵌套结构与校验测试。
- `tests/test_mqtt_topic.cpp`：topic 契约测试。
- `tests/test_registration_payload.cpp`：payload 契约测试。
- `tests/test_registration_state.cpp`：状态转换测试。
- `CMakeLists.txt`：新增核心源码和测试可执行文件。
- `docs/V1设计文档.md`：注册/发现行为和里程碑状态。

### 任务 1：迁移并校验 MQTT 嵌套配置

**文件：**
- 修改：`src/config/app_config.hpp`
- 修改：`src/config/app_config.cpp`
- 修改：`tests/test_app_config.cpp`
- 验证：`config/config.example.json`

**接口：**
- 产出：带有 `connection`、`auth` 和 `topics` 子结构的 `config::MqttConfig`。
- 产出：供后续任务使用的、已校验的 registration/telemetry suffix 和 QoS。

- [ ] **步骤 1：把测试夹具替换为嵌套结构，并增加边界失败用例**

所有需要完整 MQTT 配置的测试都使用以下合法夹具结构：

```json
"mqtt": {
  "connection": {"host": "localhost", "port": 1883, "keepalive": 60, "client_id": "cns-rpi"},
  "auth": {"username": "", "password": ""},
  "topics": {
    "namespace": "cns_rpi",
    "registration": {"suffix": "registration", "qos": 2},
    "telemetry": {"suffix": "telemetry", "qos": 0}
  }
}
```

增加表驱动 doctest 子用例，覆盖端口 `0`、keepalive `0`、QoS `-1`/`3`、空 namespace/suffix，以及包含 `/`、`+` 或 `#` 的 topic 段。每个用例都必须得到 `ConfigError::kInvalidValue`。

- [ ] **步骤 2：运行配置测试，确认旧扁平解析器会失败**

运行：

```bash
cmake --build build --target test_app_config && ./build/test_app_config
```

预期：合法嵌套夹具解析失败，因为 `broker_host` 已不再直接位于 `mqtt` 下。

- [ ] **步骤 3：引入明确的嵌套配置类型**

把扁平 `MqttConfig` 字段替换为：

```cpp
struct MqttConnectionConfig {
  std::string host;
  int port = 0;
  int keepalive_seconds = 0;
  std::string client_id_prefix;
};

struct MqttAuthConfig {
  std::string username;
  std::string password;
};

struct MqttTopicConfig {
  std::string suffix;
  int qos = 0;
};

struct MqttTopicsConfig {
  std::string topic_namespace;
  MqttTopicConfig registration;
  MqttTopicConfig telemetry;
};

struct MqttConfig {
  MqttConnectionConfig connection;
  MqttAuthConfig auth;
  MqttTopicsConfig topics;
};
```

更新注释，明确 `client_id_prefix` 表示追加 `-{vendor_id}` 前的配置项 `client_id` 值。

- [ ] **步骤 4：解析嵌套字段并执行校验**

读取准确的 JSON 键 `connection.host/port/keepalive/client_id`、`auth.username/password` 和 `topics.namespace/{registration,telemetry}.{suffix,qos}`。在 `app_config.cpp` 中增加私有辅助函数：

```cpp
bool IsValidTopicSegment(const std::string& value) {
  return !value.empty() && value.find_first_of("/+#") == std::string::npos;
}

bool IsValidQos(int qos) { return qos >= 0 && qos <= 2; }
```

解析完成后，除非 host/client ID 前缀非空、端口位于 `1..65535`、keepalive 为正数、三个 topic 段均合法且两个 QoS 均合法，否则返回 `kInvalidValue`。

- [ ] **步骤 5：运行聚焦测试**

运行：

```bash
cmake --build build --target test_app_config && ./build/test_app_config
```

预期：全部配置用例通过。

- [ ] **步骤 6：提交配置迁移**

```bash
git add config/config.example.json src/config/app_config.hpp src/config/app_config.cpp tests/test_app_config.cpp
git commit -m "feat: 迁移MQTT嵌套配置"
```

### 任务 2：新增 topic 与注册 payload 契约

**文件：**
- 修改：`src/mqtt/topic.hpp`
- 修改：`src/mqtt/topic.cpp`
- 新建：`src/registration/registration_payload.hpp`
- 新建：`src/registration/registration_payload.cpp`
- 修改：`tests/test_mqtt_topic.cpp`
- 新建：`tests/test_registration_payload.cpp`
- 修改：`CMakeLists.txt`

**接口：**
- 产出：`mqtt::BuildRegistrationTopic(namespace, vendor_id, suffix)`。
- 产出：`mqtt::BuildTelemetryTopic(namespace, vendor_id, suffix)`。
- 产出：返回紧凑 JSON 字符串的 `registration::BuildOnlinePayload(...)` 和 `BuildOfflinePayload(...)`。

- [ ] **步骤 1：编写失败的 topic 测试**

把现有双参数断言替换为：

```cpp
CHECK(mqtt::BuildRegistrationTopic("cns_rpi", "ABC123", "registration") ==
      "cns_rpi/ABC123/registration");
CHECK(mqtt::BuildTelemetryTopic("cns_rpi", "ABC123", "telemetry") ==
      "cns_rpi/ABC123/telemetry");
```

保留空字符串防御性用例，并改为传入三个参数。

- [ ] **步骤 2：运行 topic 测试并确认编译失败**

运行：

```bash
cmake --build build --target test_mqtt_topic
```

预期：因为新签名尚不存在而编译失败。

- [ ] **步骤 3：实现共享的三段式 topic 构造器**

在 `topic.cpp` 中使用一个私有辅助函数：

```cpp
std::string BuildDeviceTopic(const std::string& topic_namespace,
                             const std::string& vendor_id,
                             const std::string& suffix) {
  return topic_namespace + "/" + vendor_id + "/" + suffix;
}
```

两个公开函数都委托给该辅助函数。更新 Doxygen 注释，引用 2026-07-10 注册机制设计。

- [ ] **步骤 4：编写失败的注册 payload 测试**

测试使用 nlohmann/json 解析返回字符串，并做严格相等断言：

```cpp
registration::OnlineRegistration input{
    .vendor_id = "ABC123",
    .school_name = "NNUTC",
    .dcdw_label = "DCDW-001",
};
CHECK(nlohmann::json::parse(registration::BuildOnlinePayload(input)) == nlohmann::json{
  {"schema_version", 1}, {"vendor_id", "ABC123"}, {"school_name", "NNUTC"},
  {"dcdw_label", "DCDW-001"}, {"status", "online"}
});
```

增加 `std::nullopt` 的 online 用例，验证 `dcdw_label` 不存在；再增加 offline 用例，验证字段严格为 `schema_version`、`vendor_id` 和 `status`。

- [ ] **步骤 5：实现注册 payload 模块**

头文件契约：

```cpp
namespace registration {
struct OnlineRegistration {
  std::string vendor_id;
  std::string school_name;
  std::optional<std::string> dcdw_label;
};
std::string BuildOnlinePayload(const OnlineRegistration& input);
std::string BuildOfflinePayload(const std::string& vendor_id);
}
```

Use nlohmann/json, assign `schema_version=1`, and return `.dump()` without pretty printing.

- [ ] **步骤 6：把源码和测试加入 CMake，并运行聚焦测试**

把 `src/registration/registration_payload.cpp` 加入 `cns_rpi_core`；新增链接 `cns_rpi_core` 的 `test_registration_payload` 并注册到 CTest。

运行：

```bash
cmake -S . -B build
cmake --build build --target test_mqtt_topic test_registration_payload
./build/test_mqtt_topic
./build/test_registration_payload
```

预期：两个测试可执行文件都通过。

- [ ] **步骤 7：提交 topic 与 payload 契约**

```bash
git add CMakeLists.txt src/mqtt/topic.hpp src/mqtt/topic.cpp src/registration tests/test_mqtt_topic.cpp tests/test_registration_payload.cpp
git commit -m "feat: 新增MQTT注册消息契约"
```

### 任务 3：新增纯逻辑注册发布状态机

**文件：**
- 新建：`src/registration/registration_state.hpp`
- 新建：`src/registration/registration_state.cpp`
- 新建：`tests/test_registration_state.cpp`
- 修改：`CMakeLists.txt`

**接口：**
- 产出：`registration::RegistrationState::ShouldPublish(bool, const std::string&)`。
- 产出：`registration::RegistrationState::MarkPublished(const std::string&)`。

- [ ] **步骤 1：先编写状态转换测试**

覆盖以下准确序列：

```cpp
registration::RegistrationState state;
CHECK_FALSE(state.ShouldPublish(false, "v1"));
CHECK(state.ShouldPublish(true, "v1"));
state.MarkPublished("v1");
CHECK_FALSE(state.ShouldPublish(true, "v1"));
CHECK(state.ShouldPublish(true, "v2"));
state.MarkPublished("v2");
CHECK_FALSE(state.ShouldPublish(false, "v2"));
CHECK(state.ShouldPublish(true, "v2"));  // 重连上升沿
```

增加发布失败重试用例：不调用 `MarkPublished()`，连续两次调用 `ShouldPublish(true, "v1")`，两次都应返回 true。

- [ ] **步骤 2：运行新目标并确认尚不能构建**

运行：

```bash
cmake -S . -B build && cmake --build build --target test_registration_state
```

预期：因缺少头文件或源文件而失败。

- [ ] **步骤 3：实现最小状态机**

```cpp
class RegistrationState {
 public:
  bool ShouldPublish(bool connected, const std::string& current_payload);
  void MarkPublished(const std::string& payload);

 private:
  bool was_connected_ = false;
  bool connection_requires_publish_ = false;
  std::optional<std::string> last_published_payload_;
};
```

连接状态从 false 变为 true 时设置 `connection_requires_publish_`，每次都更新 `was_connected_`。仅在已连接且上升沿标志已设置，或当前 payload 与上次成功发布内容不同时返回 true。`MarkPublished()` 保存 payload 并清除上升沿标志。

- [ ] **步骤 4：注册测试目标并运行**

把源文件加入 `cns_rpi_core`，新建 `test_registration_state`，然后运行：

```bash
cmake -S . -B build
cmake --build build --target test_registration_state
./build/test_registration_state
```

预期：全部状态转换用例通过。

- [ ] **步骤 5：提交纯逻辑状态机**

```bash
git add CMakeLists.txt src/registration/registration_state.hpp src/registration/registration_state.cpp tests/test_registration_state.cpp
git commit -m "feat: 新增MQTT注册状态机"
```

### 任务 4：为 MqttClient 增加 Last Will 与有界发布确认

**文件：**
- 修改：`src/mqtt/mqtt_client.hpp`
- 修改：`src/mqtt/mqtt_client.cpp`
- 新建：`tests/test_mqtt_client.cpp`
- 修改：`CMakeLists.txt`

**接口：**
- 输入：由 `main.cpp` 预先构造的 Last Will topic/payload。
- 产出：`ConnectionOptions` 内的 `mqtt::WillOptions`。
- 产出：`PublishAndWait(..., std::chrono::milliseconds timeout)`。

- [ ] **步骤 1：编写失败的非法 Will 测试并增加测试目标**

新建链接 `cns_rpi_mqtt` 的 doctest：

```cpp
auto client = mqtt::MqttClient::Open({
    .broker_host = "127.0.0.1",
    .broker_port = 1,
    .client_id = "registration-test",
    .keepalive_seconds = 60,
    .will = {.topic = "invalid/#", .payload = "{}", .qos = 2, .retain = true},
});
CHECK_FALSE(client.has_value());
```

在 CMake 中注册 `test_mqtt_client`，并链接 `cns_rpi_mqtt`。

运行：

```bash
cmake -S . -B build && cmake --build build --target test_mqtt_client && ./build/test_mqtt_client
```

预期：因为 `WillOptions` 和 `ConnectionOptions::will` 尚不存在而编译失败。

- [ ] **步骤 2：增加公开接口**

增加 `<chrono>` 和以下声明：

```cpp
struct WillOptions {
  std::string topic;
  std::string payload;
  int qos = 2;
  bool retain = true;
};
```

在 `ConnectionOptions` 中增加 `WillOptions will;`，并增加：

```cpp
bool PublishAndWait(const std::string& topic, const std::string& payload, int qos,
                    bool retain, std::chrono::milliseconds timeout);
```

此时先不改变连接行为；非法 Will 测试应能编译，但会因 `Open()` 忽略非法 Will 而失败。

- [ ] **步骤 3：用稳定的客户端状态替换回调 userdata**

在 `mqtt_client.cpp` 中定义 `mqtt::ClientState`：

```cpp
struct ClientState {
  std::atomic<bool> connected{false};
  std::mutex publish_mutex;
  std::condition_variable publish_cv;
  int completed_mid = -1;
};
```

在头文件中前向声明，并保存 `std::unique_ptr<ClientState> state_`。全部回调都接收 `ClientState*`；连接/断开回调更新 `connected`，发布回调在互斥锁保护下记录 `mid`，随后通知条件变量。

- [ ] **步骤 4：在连接前设置 Last Will**

在 `Open()` 中，完成认证和回调设置后、调用 `mosquitto_connect_async()` 前执行：

```cpp
mosquitto_will_set(handle, options.will.topic.c_str(),
                   static_cast<int>(options.will.payload.size()),
                   options.will.payload.data(), options.will.qos,
                   options.will.retain);
```

任何非成功返回都销毁句柄并返回 `std::nullopt`。在启动网络循环前注册 `mosquitto_publish_callback_set()`。重新运行 `./build/test_mqtt_client`；预期通过，因为 libmosquitto 会在尝试网络连接之前拒绝包含通配符的 Will topic。

- [ ] **步骤 5：实现无回调竞态的有界发布确认**

`PublishAndWait()` must lock `publish_mutex` before calling `mosquitto_publish()`, reset `completed_mid`, capture the returned `mid`, then wait on `publish_cv` until `completed_mid == mid` or timeout. Holding the mutex during `mosquitto_publish()` ensures an immediate callback cannot be lost before the awaited `mid` is known.

发布调用报错时立即返回 false，等待超时时也返回 false；普通 `Publish()` 继续保持非阻塞。

- [ ] **步骤 6：在警告开启条件下构建 MQTT 边界**

运行：

```bash
cmake --build build --target cns_rpi_mqtt test_mqtt_client
./build/test_mqtt_client
```

预期：目标构建成功且没有新增警告。

- [ ] **步骤 7：提交 MQTT 客户端扩展**

```bash
git add CMakeLists.txt src/mqtt/mqtt_client.hpp src/mqtt/mqtt_client.cpp tests/test_mqtt_client.cpp
git commit -m "feat: MQTT客户端支持遗嘱与发布确认"
```

### 任务 5：集成注册、唯一 Client ID 与正常退出

**文件：**
- 修改：`src/main.cpp`
- 修改：`tests/test_registration_payload.cpp`

**接口：**
- 输入：嵌套 `AppConfig`、topic 构造器、注册 payload 构造器/状态机，以及扩展后的 `MqttClient`。
- 产出：完整的运行时注册/发现行为。

- [ ] **步骤 1：新增纯逻辑唯一 Client ID 构造函数及测试**

把以下函数加入注册 payload 模块，而不是 `main.cpp`：

```cpp
std::string BuildClientId(const std::string& prefix, const std::string& vendor_id);
```

`("cns-rpi", "ABC123")` 的预期输出是 `cns-rpi-ABC123`。在 `test_registration_payload.cpp` 增加此断言，运行测试并确认缺少符号，再实现 `return prefix + "-" + vendor_id;`，最后重跑至通过。

- [ ] **步骤 2：增加信号安全的退出状态**

在 `main.cpp` 文件作用域定义：

```cpp
volatile std::sig_atomic_t g_exit_requested = 0;
void HandleExitSignal(int) { g_exit_requested = 1; }
```

程序启动时使用 `std::signal` 安装 `SIGINT` 和 `SIGTERM` 处理函数。处理函数内不得记录日志或调用 MQTT。

- [ ] **步骤 3：替换旧 MQTT 配置字段访问**

使用以下准确的嵌套字段：

```cpp
app_config->mqtt.connection.host
app_config->mqtt.connection.port
app_config->mqtt.connection.keepalive_seconds
app_config->mqtt.connection.client_id_prefix
app_config->mqtt.auth.username
app_config->mqtt.auth.password
app_config->mqtt.topics.topic_namespace
app_config->mqtt.topics.registration.suffix/qos
app_config->mqtt.topics.telemetry.suffix/qos
```

使用各自配置的 suffix 构造 telemetry 和 registration topic。仅在 `vendor_id` 就绪后派生 Client ID。

- [ ] **步骤 4：创建客户端时配置 Last Will**

调用 `MqttClient::Open()` 前构造：

```cpp
const auto offline_payload = registration::BuildOfflinePayload(*snapshot.vendor_id);
```

Pass registration topic, offline payload, configured registration QoS, and `retain=true` in `ConnectionOptions::will`.

- [ ] **步骤 5：使用纯状态机驱动 online 注册**

Maintain `registration::RegistrationState registration_state;`. Each loop, build `OnlineRegistration` from current `vendor_id`, configured `school_name`, and optional `dcdw_label`. If `ShouldPublish(mqtt_client->IsConnected(), payload)` is true, call `Publish(registration_topic, payload, registration_qos, true)`. Call `MarkPublished(payload)` only when `Publish()` returns true.

在 telemetry 发布之前执行注册检查，使新连接设备优先完成声明。注册失败记录错误，但不得跳过 telemetry 逻辑。

- [ ] **步骤 6：实现正常 offline 发布和循环退出**

修改循环条件以检查 `g_exit_requested`。退出循环后，如果客户端存在且已连接，则调用：

```cpp
mqtt_client->PublishAndWait(registration_topic, offline_payload,
                            app_config->mqtt.topics.registration.qos,
                            true, std::chrono::seconds(2));
```

无论发布确认成功还是超时，都返回 `EXIT_SUCCESS`；超时日志必须在信号处理上下文之外输出。

- [ ] **步骤 7：构建全部生产目标并运行聚焦纯逻辑测试**

运行：

```bash
cmake --build build --target cns_rpi test_app_config test_mqtt_topic test_registration_payload test_registration_state
./build/test_app_config
./build/test_mqtt_topic
./build/test_registration_payload
./build/test_registration_state
```

预期：全部目标构建成功，全部聚焦测试通过。

- [ ] **步骤 8：提交运行时集成**

```bash
git add src/main.cpp src/registration/registration_payload.hpp src/registration/registration_payload.cpp tests/test_registration_payload.cpp
git commit -m "feat: 接入MQTT设备注册与离线状态"
```

### 任务 6：同步项目文档并验证完整改动

**文件：**
- 修改：`docs/V1设计文档.md`
- 验证：从计划开始后发生变化的全部文件

**接口：**
- 产出：与已实现 topic、数据库 upsert、在线状态和配置行为一致的仓库文档。

- [ ] **步骤 1：更新 V1 设计说明**

记录以下明确结论：

- Discovery subscription is `{namespace}/+/registration`.
- Per-device retained topic is `{namespace}/{vendor_id}/registration`.
- Registration is the only source for device-table metadata; telemetry does not fill `dcdw_label`.
- Online is published on connect/reconnect and identity metadata changes.
- Offline comes from Last Will or graceful shutdown.
- Client ID is `{configured_prefix}-{vendor_id}`.
- M5 registration/discovery extension is implemented; M6 remains separate.

- [ ] **步骤 2：运行完整构建和 CTest**

运行：

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

预期：构建以 0 退出且没有新增警告；所有已注册测试均通过。

- [ ] **步骤 3：校验配置和空白字符**

运行：

```bash
python3 -m json.tool config/config.example.json >/dev/null
git diff --check
```

预期：两个命令都以 0 退出。

- [ ] **步骤 4：Mosquitto CLI 可用时执行本地 broker 验证**

使用以下命令订阅：

```bash
mosquitto_sub -h localhost -t 'cns_rpi/+/registration' -v
```

使用能提供合法 `vendor_id` 的本地 UART/MAVLink 模拟器，验证 retained online、晚加入订阅者发现、角色号更新、`SIGTERM` offline、强制终止触发 Will，以及重连 online。验证只使用 localhost，不需要 5G 模块或 SIM 卡。如果本地 broker 或模拟器不可用，必须逐项记录哪些集成场景未运行，不能用成功结论代替。

- [ ] **步骤 5：提交文档和仅用于验证的修正**

```bash
git add docs/V1设计文档.md
git commit -m "docs: 补充MQTT注册发现机制"
```

- [ ] **步骤 6：检查最终提交历史和工作区**

运行：

```bash
git log -6 --oneline
git status --short
```

预期：实施提交均已存在；若仍有无关的用户改动，必须明确报告且不得静默纳入提交。
