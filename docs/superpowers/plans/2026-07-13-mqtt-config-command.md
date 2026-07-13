# MQTT 配置命令链路实施计划

> **供执行代理使用：** 必须按任务逐项执行；使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans`。所有步骤使用复选框跟踪。

**目标：** 让 RPi 从 MQTT 接收服务器规范化的运行参数命令，原子持久化完整 `config.json`，发布幂等 ACK 后正常退出，并由 systemd 拉起使新参数生效。

**架构：** `mqtt_client` 的网络线程只负责重连订阅和把消息复制进容量为 64 的队列，主线程取出消息后交给 `config_command` 模块完成解析、白名单更新、去重和持久化。启动参数选择 `disabled/direct/helper` 写入后端；成功处理结果由主循环发布 ACK 并退出，业务模块不直接控制进程生命周期。

**技术栈：** C++23、CMake、doctest、nlohmann/json、libmosquitto、POSIX `open/fsync/rename`、systemd。

## 全局约束

- 目标平台为 Raspberry Pi 5、Debian trixie、ARM64；保持 `-std=c++23 -Wall -Wextra`。
- 本仓库只实现 RPi 目标设备链路和设备来源请求发布接口，不实现 `cns_server` 数据库、路由进程或管理 API。
- 命令 topic 为 `{namespace}/{vendor_id}/config/set`，ACK topic 为 `{namespace}/{vendor_id}/config/ack`，QoS 2、`retain=false`。
- 只允许修改 `telemetry_publish_interval_ms`、`heartbeat_interval_ms`、`mqtt_reconnect_delay_s`、`mqtt_reconnect_delay_max_s`；命令必须全有或全无。
- 两个毫秒间隔范围均为 100～60000；两个 MQTT 秒间隔范围均为 1～3600，并满足 `delay_s <= delay_max_s`。
- `runtime.applied_command_ids` 只由程序维护，最多保留最近 32 个；重复命令不写盘、不退出。
- 写入后端由可信启动参数选择，默认 `disabled`；MQTT 命令不能修改模式或 helper 路径。
- MQTT 回调不得解析业务 JSON、写文件、执行 helper 或退出；队列容量固定为 64，满时丢弃新消息。
- 持久化成功后 ACK 最多等待 2 秒；无论 ACK 成功或超时都正常退出。持久化失败则拒绝命令并继续运行。
- 所有新增文档、Doxygen 注释、错误消息和提交说明使用中文；协议字段、类型名与命令行参数保留既定英文标识。
- 遵循 TDD：每个行为先写失败测试，确认失败原因正确，再做最小实现；每个任务结束后提交。

---

## 文件结构

- `src/config/app_config.hpp/.cpp`：启动配置的类型、加载和范围校验。
- `src/mqtt/topic.hpp/.cpp`：设备命令、ACK 和设备来源请求 topic 的纯字符串构造。
- `src/mqtt/mqtt_client.hpp/.cpp`：libmosquitto 连接、重连订阅、发布和有界入站队列。
- `src/config_command/command_parser.hpp/.cpp`：把服务器 JSON 解析成类型安全的白名单参数补丁。
- `src/config_command/config_updater.hpp/.cpp`：读取完整原始 JSON并生成候选 JSON，维护幂等窗口。
- `src/config_command/config_store.hpp/.cpp`：`disabled/direct/helper` 三种持久化策略。
- `src/config_command/command_processor.hpp/.cpp`：串联解析、去重、更新和写盘，返回 ACK 与退出决策。
- `src/config_command/config_request.hpp/.cpp`：构造同校设备请求 payload，接口不允许表达学校或目标 `vendor_id`。
- `src/main.cpp`：解析可信启动参数、使用配置间隔、消费 MQTT 队列、发 ACK 并退出。
- `systemd/cns-rpi.service`：最小 `Restart=always` 重启闭环。
- `tests/`：上述模块的独立 doctest 测试。

### 任务 1：把四个运行参数纳入启动配置

**文件：**
- 修改：`src/config/app_config.hpp`
- 修改：`src/config/app_config.cpp`
- 修改：`tests/test_app_config.cpp`
- 修改：`config/config.example.json`
- 修改：`src/main.cpp`
- 修改：`src/mqtt/mqtt_client.hpp`
- 修改：`src/mqtt/mqtt_client.cpp`
- 修改：`tests/test_mqtt_client.cpp`

**接口：**
- 产出：`config::RuntimeConfig`、`config::MqttReconnectConfig`；`mqtt::ConnectionOptions::{reconnect_delay_seconds,reconnect_delay_max_seconds}`。
- 消费：现有 `config::LoadAppConfig()`、`mqtt::MqttClient::Open()`。

- [ ] **步骤 1：先写配置解析失败测试和成功测试**

在 `ValidConfig()` 中加入：

```json
"runtime": {
  "telemetry_publish_interval_ms": 1000,
  "heartbeat_interval_ms": 1000,
  "applied_command_ids": []
}
```

并在 `mqtt.connection` 中加入：

```json
"reconnect": {"delay_s": 1, "delay_max_s": 30}
```

新增用例，明确覆盖边界、组合约束和 32 条上限：

```cpp
TEST_CASE("运行参数与MQTT重连参数能正确解析") {
  auto result = config::LoadAppConfig(WriteTempConfig(ValidConfig()));
  REQUIRE(result.has_value());
  CHECK(result->runtime.telemetry_publish_interval == std::chrono::milliseconds(1000));
  CHECK(result->runtime.heartbeat_interval == std::chrono::milliseconds(1000));
  CHECK(result->runtime.applied_command_ids.empty());
  CHECK(result->mqtt.connection.reconnect.delay_seconds == 1);
  CHECK(result->mqtt.connection.reconnect.delay_max_seconds == 30);
}

TEST_CASE("运行参数范围或重连组合非法时拒绝启动") {
  SUBCASE("遥测间隔低于100毫秒") {
    auto text = ReplaceOnce(ValidConfig(),
        "\"telemetry_publish_interval_ms\": 1000",
        "\"telemetry_publish_interval_ms\": 99");
    CHECK(config::LoadAppConfig(WriteTempConfig(text)).error() ==
          config::ConfigError::kInvalidValue);
  }
  SUBCASE("重连初始等待大于最大等待") {
    auto text = ReplaceOnce(ValidConfig(),
        "\"delay_s\": 1, \"delay_max_s\": 30",
        "\"delay_s\": 31, \"delay_max_s\": 30");
    CHECK(config::LoadAppConfig(WriteTempConfig(text)).error() ==
          config::ConfigError::kInvalidValue);
  }
}
```

- [ ] **步骤 2：运行测试并确认失败**

运行：`cmake --build build --target test_app_config && ./build/test_app_config`

预期：编译失败，提示 `AppConfig` 没有 `runtime`，或 `MqttConnectionConfig` 没有 `reconnect`。

- [ ] **步骤 3：加入类型和加载校验**

在 `app_config.hpp` 增加：

```cpp
#include <chrono>
#include <vector>

struct MqttReconnectConfig {
  int delay_seconds = 0;
  int delay_max_seconds = 0;
};

struct RuntimeConfig {
  std::chrono::milliseconds telemetry_publish_interval{0};
  std::chrono::milliseconds heartbeat_interval{0};
  std::vector<std::string> applied_command_ids;
};
```

把 `MqttReconnectConfig reconnect;` 加入 `MqttConnectionConfig`，把 `RuntimeConfig runtime;` 加入 `AppConfig`。在 `LoadAppConfig()` 中读取三个 `runtime` 字段和两个 `reconnect` 字段，并拒绝：超范围值、`delay_seconds > delay_max_seconds`、超过 32 个命令号、空命令号或非字符串命令号。

- [ ] **步骤 4：让业务代码消费配置值**

删除 `main.cpp` 的两个固定间隔，改用：

```cpp
if (now - last_heartbeat >= app_config->runtime.heartbeat_interval) { /* 原逻辑 */ }
if (mqtt_client && mqtt_client->IsConnected() &&
    now - last_telemetry_publish >= app_config->runtime.telemetry_publish_interval) {
  /* 原逻辑 */
}
```

在 `ConnectionOptions` 增加两个 `int` 字段，`Open()` 改为：

```cpp
if (mosquitto_reconnect_delay_set(handle, options.reconnect_delay_seconds,
                                  options.reconnect_delay_max_seconds,
                                  /*reconnect_exponential_backoff=*/true) != MOSQ_ERR_SUCCESS) {
  mosquitto_destroy(handle);
  return std::nullopt;
}
```

所有 `MqttClient::Open()` 调用点和测试显式传入 `1`、`30`，主循环传入配置值。

- [ ] **步骤 5：更新示例配置并验证**

按设计向 `config/config.example.json` 加入 `runtime` 和 `mqtt.connection.reconnect`。运行：

```bash
cmake --build build --target test_app_config test_mqtt_client cns_rpi
./build/test_app_config
./build/test_mqtt_client
```

预期：两个测试程序全部通过，`cns_rpi` 编译成功且无新增警告。

- [ ] **步骤 6：提交**

```bash
git add src/config/app_config.hpp src/config/app_config.cpp tests/test_app_config.cpp \
  config/config.example.json src/main.cpp src/mqtt/mqtt_client.hpp \
  src/mqtt/mqtt_client.cpp tests/test_mqtt_client.cpp
git commit -m "feat: 配置运行间隔与MQTT重连参数"
```

### 任务 2：补齐配置命令 Topic 契约

**文件：**
- 修改：`src/config/app_config.hpp`
- 修改：`src/config/app_config.cpp`
- 修改：`tests/test_app_config.cpp`
- 修改：`config/config.example.json`
- 修改：`src/mqtt/topic.hpp`
- 修改：`src/mqtt/topic.cpp`
- 修改：`tests/test_mqtt_topic.cpp`

**接口：**
- 产出：`MqttTopicsConfig::config_set`、`config_ack`；`BuildConfigSetTopic()`、`BuildConfigAckTopic()`、`BuildConfigRequestTopic()`。
- 消费：任务 1 的配置加载器。

- [ ] **步骤 1：写失败测试**

```cpp
TEST_CASE("构造配置命令、ACK和设备来源请求topic") {
  CHECK(mqtt::BuildConfigSetTopic("cns_rpi", "ABC123", "config/set") ==
        "cns_rpi/ABC123/config/set");
  CHECK(mqtt::BuildConfigAckTopic("cns_rpi", "ABC123", "config/ack") ==
        "cns_rpi/ABC123/config/ack");
  CHECK(mqtt::BuildConfigRequestTopic("cns_rpi", "ABC123") ==
        "cns_rpi/sources/ABC123/config/request");
}
```

配置 fixture 增加：

```json
"config_set": {"suffix": "config/set", "qos": 2},
"config_ack": {"suffix": "config/ack", "qos": 2}
```

并断言两个 suffix 与 QoS 被解析，QoS 非 2 时返回 `kInvalidValue`。

- [ ] **步骤 2：确认测试失败**

运行：`cmake --build build --target test_app_config test_mqtt_topic`

预期：编译失败，提示三个构造函数和两个配置成员不存在。

- [ ] **步骤 3：实现 topic 与配置**

设备 topic 的 suffix 允许多段固定路径，但不允许空段、`+` 或 `#`。新增：

```cpp
std::string BuildConfigSetTopic(const std::string& topic_namespace,
                                const std::string& vendor_id,
                                const std::string& suffix);
std::string BuildConfigAckTopic(const std::string& topic_namespace,
                                const std::string& vendor_id,
                                const std::string& suffix);
std::string BuildConfigRequestTopic(const std::string& topic_namespace,
                                    const std::string& source_vendor_id);
```

前两个复用 `BuildDeviceTopic()`，第三个拼接 `namespace + "/sources/" + vendor_id + "/config/request"`。配置加载器要求 `config_set.qos == 2`、`config_ack.qos == 2`。

- [ ] **步骤 4：运行测试并提交**

运行：`cmake --build build --target test_app_config test_mqtt_topic && ./build/test_app_config && ./build/test_mqtt_topic`

预期：全部通过。

```bash
git add src/config/app_config.hpp src/config/app_config.cpp tests/test_app_config.cpp \
  config/config.example.json src/mqtt/topic.hpp src/mqtt/topic.cpp tests/test_mqtt_topic.cpp
git commit -m "feat: 定义MQTT配置命令主题"
```

### 任务 3：扩展 MQTT 重连订阅与有界消息队列

**文件：**
- 修改：`src/mqtt/mqtt_client.hpp`
- 修改：`src/mqtt/mqtt_client.cpp`
- 修改：`tests/test_mqtt_client.cpp`

**接口：**
- 产出：`mqtt::IncomingMessage`、`ConnectionOptions::subscriptions`、`MqttClient::TryPopMessage()`、容量常量 `kIncomingQueueCapacity = 64`。
- 消费：任务 2 生成的完整 `config/set` topic。

- [ ] **步骤 1：写不依赖 broker 的队列失败测试**

为便于确定性单测，把队列提成 `mqtt::IncomingMessageQueue`：

```cpp
TEST_CASE("入站队列先进先出且容量固定为64") {
  mqtt::IncomingMessageQueue queue;
  for (int i = 0; i < 64; ++i) {
    CHECK(queue.Push({"topic", std::to_string(i)}));
  }
  CHECK_FALSE(queue.Push({"topic", "overflow"}));
  for (int i = 0; i < 64; ++i) {
    auto message = queue.TryPop();
    REQUIRE(message.has_value());
    CHECK(message->payload == std::to_string(i));
  }
  CHECK_FALSE(queue.TryPop().has_value());
}
```

- [ ] **步骤 2：确认测试编译失败**

运行：`cmake --build build --target test_mqtt_client`

预期：编译失败，提示 `IncomingMessageQueue` 未定义。

- [ ] **步骤 3：实现线程安全队列和订阅回调**

头文件定义：

```cpp
struct IncomingMessage { std::string topic; std::string payload; };

class IncomingMessageQueue {
 public:
  static constexpr std::size_t kCapacity = 64;
  bool Push(IncomingMessage message);
  std::optional<IncomingMessage> TryPop();
 private:
  std::mutex mutex_;
  std::deque<IncomingMessage> messages_;
};
```

`ConnectionOptions` 增加 `std::vector<std::pair<std::string, int>> subscriptions`。`ClientState` 持有订阅副本和队列；`OnConnect(rc == 0)` 遍历订阅并调用 `mosquitto_subscribe()`，确保首次连接和每次自动重连都重订阅。`OnMessage` 只复制 topic/payload 后调用 `Push()`；队列满只输出错误。`MqttClient::TryPopMessage()` 转发队列的 `TryPop()`。

- [ ] **步骤 4：增加真实 broker 集成测试入口但允许环境跳过**

保留队列测试为必跑单测；增加由 `CNS_TEST_MQTT_BROKER_PORT` 控制的测试，端口未设置时 `return`，设置时创建随机 client id，订阅测试 topic，发布消息，轮询不超过 2 秒并断言收到。该测试同时覆盖连接后订阅和消息入队，不在单测中启动/杀死系统 broker。

- [ ] **步骤 5：验证并提交**

运行：`cmake --build build --target test_mqtt_client && ./build/test_mqtt_client`

预期：队列测试通过；未设置 broker 环境变量时集成子用例安全跳过。

```bash
git add src/mqtt/mqtt_client.hpp src/mqtt/mqtt_client.cpp tests/test_mqtt_client.cpp
git commit -m "feat: 支持MQTT重连订阅与消息队列"
```

### 任务 4：解析和校验服务器配置命令

**文件：**
- 新建：`src/config_command/command_parser.hpp`
- 新建：`src/config_command/command_parser.cpp`
- 新建：`tests/test_command_parser.cpp`
- 修改：`CMakeLists.txt`

**接口：**
- 产出：`ConfigParameterPatch`、`ConfigCommand`、`CommandError`、`ParseConfigCommand(std::string_view)`、`BuildRejectedAck()`。
- 消费：nlohmann/json。

- [ ] **步骤 1：写解析失败测试**

```cpp
TEST_CASE("合法部分更新解析为类型安全补丁") {
  auto result = config_command::ParseConfigCommand(R"({
    "command_id":"cmd-001",
    "parameters":{"telemetry_publish_interval_ms":2000}
  })");
  REQUIRE(result.has_value());
  CHECK(result->command_id == "cmd-001");
  CHECK(result->parameters.telemetry_publish_interval_ms == 2000);
  CHECK_FALSE(result->parameters.heartbeat_interval_ms.has_value());
}

TEST_CASE("未知字段或任一非法字段使整条命令失败") {
  CHECK(config_command::ParseConfigCommand(
      R"({"command_id":"cmd-1","parameters":{"serial_baud":9600}})")
      .error().code == "unknown_parameter");
  CHECK(config_command::ParseConfigCommand(
      R"({"command_id":"cmd-1","parameters":{"heartbeat_interval_ms":99}})")
      .error().code == "invalid_parameter");
}
```

另覆盖非法 JSON、空/非字符串/超过 128 字节的 `command_id`、空 `parameters`、类型错误、四个字段上下边界和 `delay_s <= delay_max_s`。

- [ ] **步骤 2：确认测试失败**

在 CMake 增加 `test_command_parser` 目标后运行：`cmake -S . -B build && cmake --build build --target test_command_parser`

预期：编译失败，提示头文件或接口不存在。

- [ ] **步骤 3：实现纯解析器**

```cpp
struct ConfigParameterPatch {
  std::optional<int> telemetry_publish_interval_ms;
  std::optional<int> heartbeat_interval_ms;
  std::optional<int> mqtt_reconnect_delay_s;
  std::optional<int> mqtt_reconnect_delay_max_s;
};

struct ConfigCommand {
  std::string command_id;
  ConfigParameterPatch parameters;
};

struct CommandError { std::string code; std::string message; };

std::expected<ConfigCommand, CommandError> ParseConfigCommand(std::string_view payload);
nlohmann::json BuildRejectedAck(std::string_view command_id, const CommandError& error);
```

解析时先校验顶层恰有 `command_id`、`parameters`，再遍历参数键并逐个做精确整数类型和范围检查。若补丁同时给两个重连值，直接检查组合关系；只给一个值时留给更新器结合原配置检查。

- [ ] **步骤 4：验证并提交**

运行：`cmake --build build --target test_command_parser && ./build/test_command_parser`

预期：全部通过。

```bash
git add CMakeLists.txt src/config_command/command_parser.hpp \
  src/config_command/command_parser.cpp tests/test_command_parser.cpp
git commit -m "feat: 解析并校验配置命令"
```

### 任务 5：生成完整候选配置并维护幂等窗口

**文件：**
- 新建：`src/config_command/config_updater.hpp`
- 新建：`src/config_command/config_updater.cpp`
- 新建：`tests/test_config_updater.cpp`
- 修改：`CMakeLists.txt`

**接口：**
- 产出：`LoadConfigJson()`、`IsCommandApplied()`、`BuildUpdatedConfig()`。
- 消费：任务 4 的 `ConfigCommand`。

- [ ] **步骤 1：写白名单更新和去重失败测试**

```cpp
TEST_CASE("部分更新保留所有非白名单字段并追加命令号") {
  auto root = nlohmann::json::parse(ValidRawConfig());
  config_command::ConfigCommand command{
      .command_id = "cmd-002",
      .parameters = {.telemetry_publish_interval_ms = 2000}};
  auto result = config_command::BuildUpdatedConfig(root, command);
  REQUIRE(result.has_value());
  CHECK((*result)["runtime"]["telemetry_publish_interval_ms"] == 2000);
  CHECK((*result)["serial"] == root["serial"]);
  CHECK((*result)["custom_future_field"] == root["custom_future_field"]);
  CHECK((*result)["runtime"]["applied_command_ids"].back() == "cmd-002");
}

TEST_CASE("幂等窗口只保留最近32条") {
  auto root = nlohmann::json::parse(ConfigWithAppliedIds(32));
  auto result = config_command::BuildUpdatedConfig(root, Command("cmd-032"));
  REQUIRE(result.has_value());
  CHECK((*result)["runtime"]["applied_command_ids"].size() == 32);
  CHECK((*result)["runtime"]["applied_command_ids"].front() == "cmd-001");
  CHECK((*result)["runtime"]["applied_command_ids"].back() == "cmd-032");
}
```

另覆盖：原配置打不开/非法 JSON；只更新 `delay_s` 后与原 `delay_max_s` 冲突；重复 `command_id` 被 `IsCommandApplied()` 识别。

- [ ] **步骤 2：确认测试失败**

运行：`cmake -S . -B build && cmake --build build --target test_config_updater`

预期：编译失败，提示更新器接口不存在。

- [ ] **步骤 3：实现完整 JSON 更新**

```cpp
std::expected<nlohmann::json, CommandError> LoadConfigJson(
    const std::filesystem::path& path);
bool IsCommandApplied(const nlohmann::json& root, std::string_view command_id);
std::expected<nlohmann::json, CommandError> BuildUpdatedConfig(
    const nlohmann::json& current, const ConfigCommand& command);
```

从 `current` 复制得到候选对象，只写四个映射路径；只给一个重连值时用候选对象中的另一个值检查最终组合约束。最后追加并裁剪 `applied_command_ids`。不通过 `AppConfig` 重新序列化，以免丢失 `cellular` 和未来未知字段。

- [ ] **步骤 4：验证并提交**

运行：`cmake --build build --target test_config_updater && ./build/test_config_updater`

预期：全部通过。

```bash
git add CMakeLists.txt src/config_command/config_updater.hpp \
  src/config_command/config_updater.cpp tests/test_config_updater.cpp
git commit -m "feat: 生成配置更新与幂等记录"
```

### 任务 6：实现可信启动参数和三种配置写入后端

**文件：**
- 新建：`src/config_command/config_store.hpp`
- 新建：`src/config_command/config_store.cpp`
- 新建：`tests/test_config_store.cpp`
- 修改：`CMakeLists.txt`
- 修改：`src/main.cpp`

**接口：**
- 产出：`ConfigWriterMode`、`WriterOptions`、`ParseWriterOptions()`、`PersistConfig()`。
- 消费：候选 `nlohmann::json` 和配置文件路径。

- [ ] **步骤 1：写模式解析和 direct 写入失败测试**

```cpp
TEST_CASE("写入模式默认disabled且helper必须有固定路径") {
  CHECK(config_command::ParseWriterOptions({}).value().mode ==
        config_command::ConfigWriterMode::kDisabled);
  auto direct = config_command::ParseWriterOptions({"--config-writer=direct"});
  REQUIRE(direct.has_value());
  CHECK(direct->mode == config_command::ConfigWriterMode::kDirect);
  CHECK_FALSE(config_command::ParseWriterOptions({"--config-writer=helper"}).has_value());
}

TEST_CASE("direct原子替换完整JSON") {
  TempDirectory dir;
  auto path = dir.path() / "config.json";
  WriteText(path, R"({"old":true})");
  auto result = config_command::PersistConfig(
      {.mode = config_command::ConfigWriterMode::kDirect}, path,
      nlohmann::json{{"new", true}});
  REQUIRE(result.has_value());
  CHECK(nlohmann::json::parse(ReadText(path)) == nlohmann::json{{"new", true}});
  CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
}
```

另覆盖 `disabled -> config_write_disabled`、目标目录不可写时原文件不变、helper 非零退出 -> `config_write_failed`、未知或重复启动参数拒绝启动。

- [ ] **步骤 2：确认测试失败**

运行：`cmake -S . -B build && cmake --build build --target test_config_store`

预期：编译失败，提示 store 接口不存在。

- [ ] **步骤 3：实现 direct 原子写入**

```cpp
enum class ConfigWriterMode { kDisabled, kDirect, kHelper };
struct WriterOptions {
  ConfigWriterMode mode = ConfigWriterMode::kDisabled;
  std::filesystem::path helper_path;
};

std::expected<WriterOptions, std::string> ParseWriterOptions(
    const std::vector<std::string_view>& arguments);
std::expected<void, CommandError> PersistConfig(
    const WriterOptions& options, const std::filesystem::path& config_path,
    const nlohmann::json& candidate);
```

`direct` 在同目录以固定前缀和进程 ID 创建独占临时文件，写入 `candidate.dump(2) + "\n"`，依次检查 `write`、文件 `fsync`、`close`、`rename`、目录 `fsync`。任何失败关闭并删除临时文件，保留旧文件；不要用跨文件系统的系统临时目录。

- [ ] **步骤 4：实现 helper 模式且不调用 shell**

先把同目录候选文件按 direct 的“写入 + 文件 fsync”步骤准备好，然后用 `fork` + `execv` 直接执行：

```text
{helper_path} {candidate_path} {config_path}
```

不使用 `system()`，不拼 shell 命令。等待子进程；退出码 0 后对目标目录 `fsync` 并确认目标文件可解析，非 0、信号退出或 `execv` 失败均返回 `config_write_failed`，并清理候选文件。

- [ ] **步骤 5：主程序解析可信参数**

位置参数仍只允许零个或一个配置路径；从其余参数解析 `--config-writer=` 和 `--config-helper=`。非法组合在打开串口和 MQTT 前打印中文错误并返回 `EXIT_FAILURE`。

- [ ] **步骤 6：验证并提交**

运行：`cmake --build build --target test_config_store cns_rpi && ./build/test_config_store`

预期：全部通过，无新增编译警告。

```bash
git add CMakeLists.txt src/config_command/config_store.hpp \
  src/config_command/config_store.cpp tests/test_config_store.cpp src/main.cpp
git commit -m "feat: 支持配置持久化后端"
```

### 任务 7：编排命令处理、ACK 与退出决策

**文件：**
- 新建：`src/config_command/command_processor.hpp`
- 新建：`src/config_command/command_processor.cpp`
- 新建：`tests/test_command_processor.cpp`
- 修改：`CMakeLists.txt`

**接口：**
- 产出：`CommandProcessResult`、`ProcessConfigCommand()`。
- 消费：任务 4～6 的解析器、更新器和存储后端。

- [ ] **步骤 1：通过可替换存储函数写编排失败测试**

```cpp
TEST_CASE("成功持久化返回applied并要求退出") {
  bool persisted = false;
  auto result = config_command::ProcessConfigCommand(
      ValidPayload("cmd-new"), CurrentConfig(),
      [&](const nlohmann::json&) {
        persisted = true;
        return std::expected<void, config_command::CommandError>{};
      });
  CHECK(persisted);
  CHECK(result.ack == nlohmann::json{{"command_id", "cmd-new"},
                                     {"status", "applied"},
                                     {"restart_required", true}});
  CHECK(result.should_exit);
}

TEST_CASE("重复命令不写盘不退出") {
  bool persisted = false;
  auto result = config_command::ProcessConfigCommand(
      ValidPayload("cmd-old"), ConfigContaining("cmd-old"),
      [&](const nlohmann::json&) { persisted = true; return Success(); });
  CHECK_FALSE(persisted);
  CHECK(result.ack["status"] == "already_applied");
  CHECK_FALSE(result.should_exit);
}
```

另覆盖解析失败、配置读取失败、持久化失败；所有失败 ACK 必须包含设计中的错误码、中文消息和 `restart_required=false`。

- [ ] **步骤 2：确认测试失败**

运行：`cmake -S . -B build && cmake --build build --target test_command_processor`

预期：编译失败，提示 processor 接口不存在。

- [ ] **步骤 3：实现无 MQTT 依赖的处理器**

```cpp
struct CommandProcessResult {
  nlohmann::json ack;
  bool should_exit = false;
};

using PersistFunction = std::function<std::expected<void, CommandError>(
    const nlohmann::json&)>;

CommandProcessResult ProcessConfigCommand(
    std::string_view payload, const nlohmann::json& current,
    const PersistFunction& persist);
```

严格按“解析 → 去重 → 构建候选 → 持久化 → ACK/退出决策”执行。持久化调用返回成功前绝不生成 `applied`，也不要求退出。

- [ ] **步骤 4：验证并提交**

运行：`cmake --build build --target test_command_processor && ./build/test_command_processor`

预期：全部通过。

```bash
git add CMakeLists.txt src/config_command/command_processor.hpp \
  src/config_command/command_processor.cpp tests/test_command_processor.cpp
git commit -m "feat: 编排配置命令执行与ACK"
```

### 任务 8：预留设备同校配置请求发布接口

**文件：**
- 新建：`src/config_command/config_request.hpp`
- 新建：`src/config_command/config_request.cpp`
- 新建：`tests/test_config_request.cpp`
- 修改：`CMakeLists.txt`

**接口：**
- 产出：`BuildConfigRequestPayload()` 和自由函数 `PublishConfigRequest()`。
- 消费：任务 2 的 `BuildConfigRequestTopic()`、任务 4 的 `ConfigParameterPatch`。

- [ ] **步骤 1：写接口边界失败测试**

```cpp
TEST_CASE("设备请求只能表达内部编号目标") {
  auto payload = config_command::BuildConfigRequestPayload(
      "req-001", "DCDW-002", {.heartbeat_interval_ms = 2000});
  REQUIRE(payload.has_value());
  CHECK((*payload)["target"] == nlohmann::json{{"dcdw_label", "DCDW-002"}});
  CHECK_FALSE(payload->contains("source_id"));
  CHECK_FALSE((*payload)["target"].contains("school_name"));
  CHECK_FALSE((*payload)["target"].contains("vendor_id"));
}
```

另覆盖空/超长 `request_id`、不符合 `DCDW-[0-9]{3}` 的目标和空补丁。

- [ ] **步骤 2：确认测试失败**

运行：`cmake -S . -B build && cmake --build build --target test_config_request`

预期：编译失败，提示请求构造接口不存在。

- [ ] **步骤 3：实现构造器和独立发布薄封装**

```cpp
std::expected<nlohmann::json, CommandError> BuildConfigRequestPayload(
    std::string_view request_id, std::string_view target_dcdw_label,
    const ConfigParameterPatch& parameters);
```

在 `config_request.hpp` 前向声明 `mqtt::MqttClient`，增加：

```cpp
bool PublishConfigRequest(mqtt::MqttClient& client,
                          const std::string& topic_namespace,
                          const std::string& source_vendor_id,
                          const std::string& request_id,
                          const std::string& target_dcdw_label,
                          const config_command::ConfigParameterPatch& parameters);
```

实现固定调用 `BuildConfigRequestTopic(topic_namespace, source_vendor_id)` 和通用 `client.Publish()`，QoS 2、`retain=false`。不提供目标学校和目标 vendor 参数。本轮不在 `main.cpp` 增加调用入口，也不让底层 `mqtt_client` 反向依赖 `config_command` 业务类型。CMake 新增 `cns_rpi_config_request` 小型库并链接 `cns_rpi_core`、`cns_rpi_mqtt`，测试只链接该库。

- [ ] **步骤 4：验证并提交**

运行：`cmake --build build --target test_config_request test_mqtt_client && ./build/test_config_request && ./build/test_mqtt_client`

预期：全部通过。

```bash
git add CMakeLists.txt src/config_command/config_request.hpp \
  src/config_command/config_request.cpp tests/test_config_request.cpp
git commit -m "feat: 预留同校配置请求接口"
```

### 任务 9：接入主循环和最小 systemd 重启闭环

**文件：**
- 修改：`src/main.cpp`
- 新建：`systemd/cns-rpi.service`
- 新建：`tests/test_cns_rpi_service.cpp`
- 修改：`CMakeLists.txt`
- 修改：`docs/MQTT命令路由与运行时配置设计.md`
- 修改：`docs/V1设计文档.md`

**接口：**
- 消费：任务 2 的命令/ACK topic，任务 3 的消息队列，任务 6 的 writer 选项，任务 7 的处理器。
- 产出：端到端设备侧命令执行闭环。

- [ ] **步骤 1：先写 service 契约测试**

```cpp
TEST_CASE("systemd服务启用正常退出重启") {
  const auto text = ReadText(SOURCE_DIR "/systemd/cns-rpi.service");
  CHECK(text.find("Restart=always") != std::string::npos);
  CHECK(text.find("RestartSec=2") != std::string::npos);
  CHECK(text.find("--config-writer=helper") != std::string::npos);
  CHECK(text.find("--config-helper=/usr/local/libexec/cns-rpi-apply-config") !=
        std::string::npos);
}
```

CMake 为该测试注入 `SOURCE_DIR="${CMAKE_SOURCE_DIR}"`。

- [ ] **步骤 2：确认测试失败**

运行：`cmake -S . -B build && cmake --build build --target test_cns_rpi_service && ./build/test_cns_rpi_service`

预期：失败，提示服务文件不存在或缺少所需字段。

- [ ] **步骤 3：在连接时订阅目标命令 topic**

身份就绪后同时构造：

```cpp
config_set_topic = mqtt::BuildConfigSetTopic(
    topics.topic_namespace, *snapshot.vendor_id, topics.config_set.suffix);
config_ack_topic = mqtt::BuildConfigAckTopic(
    topics.topic_namespace, *snapshot.vendor_id, topics.config_ack.suffix);
```

把 `{config_set_topic, 2}` 传入 `ConnectionOptions::subscriptions`。vendor ID 变化重建客户端时同步清空两个 topic。

- [ ] **步骤 4：主线程消费消息、发布 ACK 并决定退出**

每轮最多取一条消息，先验证 `message.topic == config_set_topic`。读取当前完整 JSON；读取失败构造 `config_read_failed` ACK。否则调用处理器，持久化 lambda 捕获 writer 选项和 `config_path`。ACK 使用：

```cpp
const bool acked = mqtt_client->PublishAndWait(
    config_ack_topic, result.ack.dump(), app_config->mqtt.topics.config_ack.qos,
    /*retain=*/false, std::chrono::seconds(2));
if (!acked) {
  std::cerr << "配置命令ACK发布失败或超时" << std::endl;
}
if (result.should_exit) {
  g_exit_requested = 1;
}
```

只有 `result.should_exit` 为真时才退出；`already_applied` 和所有 rejected 结果继续运行。

- [ ] **步骤 5：提供最小服务文件**

```ini
[Unit]
Description=CNS RPi 数据节点
After=network-online.target cellular-dialup.service
Wants=network-online.target

[Service]
Type=simple
User=dcdw
WorkingDirectory=/home/dcdw/cns_rpi
ExecStart=/home/dcdw/cns_rpi/build/cns_rpi /home/dcdw/cns_rpi/config/config.json --config-writer=helper --config-helper=/usr/local/libexec/cns-rpi-apply-config
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
```

- [ ] **步骤 6：更新文档状态**

把 `docs/MQTT命令路由与运行时配置设计.md` 状态更新为“已实施”，并在实现范围注明服务器侧仍待服务器仓库实施。同步 `docs/V1设计文档.md` 的配置参数来源、MQTT 下行配置命令和最小 systemd 服务现状，不把 M6 电机命令误标为完成。

- [ ] **步骤 7：运行全量验证**

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
git diff --check
```

预期：所有测试通过，`git diff --check` 无输出，编译无新增警告。

- [ ] **步骤 8：提交**

```bash
git add CMakeLists.txt src/main.cpp systemd/cns-rpi.service \
  tests/test_cns_rpi_service.cpp docs/MQTT命令路由与运行时配置设计.md \
  docs/V1设计文档.md
git commit -m "feat: 接入MQTT配置命令重启闭环"
```

### 任务 10：本地 MQTT、systemd 与 Raspberry Pi ARM64 验收

**文件：**
- 修改：`docs/MQTT命令路由与运行时配置设计.md`（只记录实际验证结果）

**接口：**
- 消费：任务 1～9 的完整程序。
- 产出：可复核的设备侧验收证据。

- [ ] **步骤 1：本地 Mosquitto 验证 direct 模式**

复制示例配置到临时目录并指向本地 broker，使用可用伪终端或现有 MAVLink 模拟工具让程序获得 vendor ID。以 `--config-writer=direct` 启动后向实际 `config/set` topic 发布：

```json
{"command_id":"acceptance-001","parameters":{"telemetry_publish_interval_ms":2000,"mqtt_reconnect_delay_max_s":60}}
```

预期：收到 QoS 2、非 retained 的 `applied` ACK；配置文件保留原有非白名单字段并追加 `acceptance-001`；程序正常退出。

- [ ] **步骤 2：验证重复与非法命令**

重新启动后再次发送 `acceptance-001`，预期 `already_applied` 且进程不退出。发送超范围参数和未知字段，预期分别得到 `invalid_parameter`、`unknown_parameter`，配置文件摘要不变，进程不退出。

- [ ] **步骤 3：验证 ACK 丢失语义**

让 ACK 发布失败或使用不可达 broker 触发 2 秒超时。预期：只要配置已经持久化，进程仍正常退出；重启后同一 `command_id` 被判定为 `already_applied`。

- [ ] **步骤 4：在 Raspberry Pi 5 ARM64 构建并测试**

将提交同步到树莓派测试工作区后运行：

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

预期：ARM64 全量构建成功、所有测试通过。若产品 helper 和 OverlayFS 尚未部署，只验证 helper 的测试替身；真实 OverlayFS 断电持久化留给 M7 部署验收，不能虚报完成。

- [ ] **步骤 5：记录证据并提交**

只把实际执行的命令、日期、平台和结果写入设计文档“验证记录”；未执行项明确标记“待 M7”。

```bash
git add docs/MQTT命令路由与运行时配置设计.md
git commit -m "docs: 记录配置命令链路验证结果"
```
