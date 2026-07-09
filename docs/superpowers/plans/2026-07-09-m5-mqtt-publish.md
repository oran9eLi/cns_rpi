# M5 MQTT发布 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** RPi 在身份就绪（`state_store.vendor_id` 解出值）后连接 MQTT broker，按 1Hz 固定节奏把 `payload::ToJson()` 的结果发布到 `{topic_prefix}/{vendor_id}/telemetry`，断线交给 libmosquitto 自动重连。

**Architecture:** 新增独立 `mqtt/` 模块，拆成两个文件：`topic.hpp/.cpp`（纯字符串拼接，不依赖 libmosquitto，跟 `cns_rpi_core` 一起编译）和 `mqtt_client.hpp/.cpp`（包一层 libmosquitto RAII 客户端，单独一个 `cns_rpi_mqtt` 库目标，只有它需要链接真实的 libmosquitto）。`main.cpp` 在主循环里加两个轮询判断（连接判断、发布判断），跟现有驱动 UART 心跳发送的 `kHeartbeatInterval` 节拍器同一个模式，不新增线程。

**Tech Stack:** C++23，libmosquitto（异步连接 + 自动重连 + 后台网络线程），nlohmann/json（复用 M4 的 `payload::ToJson`），doctest（`mqtt/topic.hpp` 的纯逻辑单测）。

## Global Constraints

- C++23，`-Wall -Wextra` 编译无警告（`CMakeLists.txt` 已全局开启）。
- 每个新文件的文件头用 Doxygen 风格中文注释，说明这个文件负责什么/依赖边界（"只依赖 XXX，不包含 YYY"）——这是本仓库既有约定，不是新规矩。
- Commit message 格式 `<type>: <简短中文说明>`，中文描述，不写长正文（`docs/协作规则.md`）。
- 不改 `config/config.json`/`config.example.json` 的 schema——`mqtt`/`identity` 两节字段在 M2/M4 已经齐了，本次全部复用。
- `mqtt/mqtt_client.cpp`（真连 libmosquitto 的部分）不写自动化 ctest 单测——本仓库对"真连外部系统"的代码一贯不写自动化单测（`uart::MavlinkLink` 对真实串口是先例），这次维持同样的边界，改用人工验证步骤；`mqtt/topic.hpp`（纯逻辑）正常写 doctest 单测。
- Task 2 起需要本机装好 `libmosquitto-dev`（`sudo apt install libmosquitto-dev`）才能编译，Task 3 的人工验证还需要一个能连的 broker（`sudo apt install mosquitto`，本机自装，见 Task 3）。
- 依据：`docs/superpowers/specs/2026-07-09-m5-mqtt-publish-design.md`（本计划的设计文档，所有字段/行为细节以它为准，本文档不重复背景，只给可执行步骤）。

---

## Task 1: `mqtt::BuildTelemetryTopic`（topic 命名，纯逻辑）

新建 `mqtt/` 模块的第一个文件——不依赖 libmosquitto，本机随时能编译/单测。

**Files:**
- Create: `src/mqtt/topic.hpp`
- Create: `src/mqtt/topic.cpp`
- Create: `tests/test_mqtt_topic.cpp`
- Modify: `CMakeLists.txt`（新增 `src/mqtt/topic.cpp` 到 `cns_rpi_core`，新增 `test_mqtt_topic` target）

**Interfaces:**
- Produces: `mqtt::BuildTelemetryTopic(const std::string& topic_prefix, const std::string& vendor_id) -> std::string`——Task 3 的 `main.cpp` 会调用它。

- [ ] **Step 1: 写 `src/mqtt/topic.hpp`**

```cpp
#pragma once

/**
 * @file topic.hpp
 * @brief MQTT topic 命名规则——纯字符串拼接，不依赖 libmosquitto。
 *
 * @details
 * 拆成独立文件是为了让这部分逻辑本机不装 libmosquitto-dev 也能编译/单测
 * （对比 mqtt_client.hpp/.cpp 必须链接真实的 libmosquitto）。
 * 命名方案见 docs/superpowers/specs/2026-07-09-m5-mqtt-publish-design.md §2。
 * 依赖边界：只依赖标准库，不包含 mosquitto.h/state/ 等其他模块头文件。
 */

#include <string>

namespace mqtt {

/**
 * @brief 拼遥测发布 topic。
 * @param topic_prefix 来自 config.json 的 mqtt.topic_prefix（区分环境/产品线）。
 * @param vendor_id 厂商唯一产品识别码（docs/设备标识符.md 权威全局键，来自
 * state_store.vendor_id，调用方需保证已经有值才调用本函数）。
 * @return "{topic_prefix}/{vendor_id}/telemetry"。不做参数校验（空字符串只是
 * 拼出带空段的字符串，不是这个函数要处理的错误）。
 */
std::string BuildTelemetryTopic(const std::string& topic_prefix, const std::string& vendor_id);

}  // namespace mqtt
```

- [ ] **Step 2: 写 `src/mqtt/topic.cpp`**

```cpp
/**
 * @file topic.cpp
 * @brief topic.hpp 的实现。
 */

#include "mqtt/topic.hpp"

namespace mqtt {

std::string BuildTelemetryTopic(const std::string& topic_prefix, const std::string& vendor_id) {
  return topic_prefix + "/" + vendor_id + "/telemetry";
}

}  // namespace mqtt
```

- [ ] **Step 3: 写 `tests/test_mqtt_topic.cpp`**（骨架任务，测试写完即通过，不是先失败后修复——`topic.cpp` 已经在 Step 2 写完实现）

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "mqtt/topic.hpp"

TEST_CASE("BuildTelemetryTopic按{topic_prefix}/{vendor_id}/telemetry拼接") {
  CHECK(mqtt::BuildTelemetryTopic("cns_rpi", "DCDWCNS1ABCDEFGHIJKL") ==
        "cns_rpi/DCDWCNS1ABCDEFGHIJKL/telemetry");
}

TEST_CASE("BuildTelemetryTopic对空topic_prefix仍正常拼接(防御性场景，不做校验)") {
  CHECK(mqtt::BuildTelemetryTopic("", "DCDWCNS1ABCDEFGHIJKL") == "/DCDWCNS1ABCDEFGHIJKL/telemetry");
}

TEST_CASE("BuildTelemetryTopic对空vendor_id仍正常拼接(防御性场景，不做校验)") {
  CHECK(mqtt::BuildTelemetryTopic("cns_rpi", "") == "cns_rpi//telemetry");
}
```

- [ ] **Step 4: 改 `CMakeLists.txt`**

在 `add_library(cns_rpi_core ...)` 的源文件列表末尾（`src/payload/json_serializer.cpp` 后面）加一行：

```cmake
    src/mqtt/topic.cpp
```

在文件末尾（`test_json_serializer` 那段之后）新增：

```cmake
add_executable(test_mqtt_topic tests/test_mqtt_topic.cpp)
target_link_libraries(test_mqtt_topic PRIVATE cns_rpi_core)
add_test(NAME mqtt_topic COMMAND test_mqtt_topic)
```

- [ ] **Step 5: 编译并运行测试**

```bash
cmake --build build
ctest --test-dir build --output-on-failure -R mqtt_topic
```

Expected: 三条 `TEST_CASE` 全部 PASS。

- [ ] **Step 6: Commit**

```bash
git add src/mqtt/topic.hpp src/mqtt/topic.cpp tests/test_mqtt_topic.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: 新增mqtt/topic拼接遥测topic

M5第一步：mqtt::BuildTelemetryTopic()拼{topic_prefix}/{vendor_id}/telemetry，
纯字符串逻辑，不依赖libmosquitto，本机随时能编译/单测。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `mqtt::MqttClient`（libmosquitto 客户端，RAII 包装）

**前置条件**：本机需要先装 `libmosquitto-dev`（`sudo apt install libmosquitto-dev`），否则本 Task 的 CMake 配置/编译步骤会失败——这是环境准备，不是这个 Task 的产出，装完再开始。

**Files:**
- Create: `src/mqtt/mqtt_client.hpp`
- Create: `src/mqtt/mqtt_client.cpp`
- Modify: `CMakeLists.txt`（新增 `find_package(PkgConfig)` + `pkg_check_modules` 找 libmosquitto，新增独立的 `cns_rpi_mqtt` 库目标）

**Interfaces:**
- Consumes: 无（这个文件不依赖前面任何 Task 的产出，只依赖标准库和 libmosquitto）。
- Produces: `mqtt::ConnectionOptions` 结构体（`broker_host`/`broker_port`/`client_id`/`username`/`password`/`keepalive_seconds`）、`mqtt::MqttClient` 类（`Open`/`Publish`/`IsConnected`）——Task 3 的 `main.cpp` 会用到这三个。

**本 Task 没有自动化单测**（Global Constraints 已说明原因），验收标准是"编译通过"，Step 4 只编译不跑测试。

- [ ] **Step 1: 写 `src/mqtt/mqtt_client.hpp`**

```cpp
#pragma once

/**
 * @file mqtt_client.hpp
 * @brief 包一层 libmosquitto：连接/自动重连/发布，不涉及 topic 命名、不涉及
 * 何时连接/何时发布的业务判断（那是 main.cpp 的事，见
 * docs/superpowers/specs/2026-07-09-m5-mqtt-publish-design.md §6）。
 *
 * @details
 * V1 只发布，不订阅——命令下行/ACK 回传是 M6 的事，这里不暴露任何订阅接口。
 * 断线重连交给 libmosquitto 自己的后台线程（mosquitto_loop_start +
 * mosquitto_reconnect_delay_set），这个类不实现应用层重试逻辑。
 * 用前向声明 struct mosquitto，不在头文件里包含 <mosquitto.h>——这样只
 * 声明/调用这个类接口的调用方（main.cpp）不需要 libmosquitto 的公开头文件
 * 也能编译，只有 mqtt_client.cpp 本身需要真正链接 libmosquitto。
 * 依赖边界：只依赖标准库，不包含 state/、payload/、config/ 等其他模块头文件
 * （ConnectionOptions 的字段是从 config::MqttConfig 摘出来的原始值，不是
 * 直接依赖 config::MqttConfig 这个类型，调用方在 main.cpp 里做字段搬运）。
 */

#include <atomic>
#include <memory>
#include <optional>
#include <string>

struct mosquitto;

namespace mqtt {

/// 连接一个 MQTT broker 所需的全部参数，字段跟 config::MqttConfig 一一对应
/// （去掉了 mqtt_client.hpp 用不到的 topic_prefix/qos——那两个是 main.cpp
/// 拼 topic/传 Publish 参数时用的，不是"建连接"本身需要的）。
struct ConnectionOptions {
  std::string broker_host;
  int broker_port = 0;
  std::string client_id;
  std::string username;
  std::string password;
  int keepalive_seconds = 0;
};

/// libmosquitto 客户端的 RAII 包装：一个实例对应一条到 broker 的连接。
class MqttClient {
 public:
  /**
   * @brief 创建客户端、发起异步连接、启动后台网络线程。
   * @details 连接是否已经建立成功不由这个函数保证——mosquitto_connect_async
   * 只是发起连接，真正连上是异步的，之后要用 IsConnected() 轮询。
   * @return 客户端对象创建/连接发起失败（mosquitto_new/mosquitto_connect_async/
   * mosquitto_loop_start 任一步返回错误）时返回 std::nullopt；否则返回一个还在
   * "连接中"或已经连上的客户端（具体状态由 IsConnected() 反映）。
   */
  static std::optional<MqttClient> Open(const ConnectionOptions& options);

  ~MqttClient();
  MqttClient(MqttClient&&) noexcept;
  MqttClient& operator=(MqttClient&&) noexcept;
  MqttClient(const MqttClient&) = delete;
  MqttClient& operator=(const MqttClient&) = delete;

  /**
   * @brief 发布一条消息。
   * @details 很薄的一层转发，不判断当前是否已连接（调用方应先用 IsConnected()
   * 把关，未连接时调用本函数本身不会崩溃，只会返回 false）。
   * @param topic 完整 topic 字符串（调用方用 mqtt::BuildTelemetryTopic() 拼好传入）。
   * @param payload 消息体，原样按字节发送（本项目里恒为 JSON 文本）。
   * @param qos 服务质量等级（0/1/2），来自 config.json 的 mqtt.qos。
   * @param retain 是否让 broker 保留这条消息给之后的新订阅者。
   * @return 发布调用成功返回 true，libmosquitto 返回非 MOSQ_ERR_SUCCESS 则 false。
   */
  bool Publish(const std::string& topic, const std::string& payload, int qos, bool retain);

  /// 读取当前连接状态（由 on_connect/on_disconnect 回调维护，不主动问库）。
  bool IsConnected() const;

 private:
  MqttClient(mosquitto* handle, std::shared_ptr<std::atomic<bool>> connected);

  mosquitto* handle_;
  std::shared_ptr<std::atomic<bool>> connected_;
};

}  // namespace mqtt
```

- [ ] **Step 2: 写 `src/mqtt/mqtt_client.cpp`**

```cpp
/**
 * @file mqtt_client.cpp
 * @brief mqtt_client.hpp 的实现。
 */

#include "mqtt/mqtt_client.hpp"

#include <iostream>
#include <mutex>
#include <utility>

#include <mosquitto.h>

namespace mqtt {

namespace {

void OnConnect(struct mosquitto* /*mosq*/, void* userdata, int rc) {
  auto* connected = static_cast<std::atomic<bool>*>(userdata);
  if (rc == 0) {
    connected->store(true);
    std::cout << "MQTT已连接" << std::endl;
  } else {
    std::cerr << "MQTT连接失败: rc=" << rc << std::endl;
  }
}

void OnDisconnect(struct mosquitto* /*mosq*/, void* userdata, int /*rc*/) {
  auto* connected = static_cast<std::atomic<bool>*>(userdata);
  connected->store(false);
  std::cout << "MQTT连接断开" << std::endl;
}

}  // namespace

MqttClient::MqttClient(mosquitto* handle, std::shared_ptr<std::atomic<bool>> connected)
    : handle_(handle), connected_(std::move(connected)) {}

MqttClient::MqttClient(MqttClient&& other) noexcept
    : handle_(other.handle_), connected_(std::move(other.connected_)) {
  other.handle_ = nullptr;
}

MqttClient& MqttClient::operator=(MqttClient&& other) noexcept {
  if (this != &other) {
    if (handle_) {
      mosquitto_loop_stop(handle_, /*force=*/true);
      mosquitto_destroy(handle_);
    }
    handle_ = other.handle_;
    connected_ = std::move(other.connected_);
    other.handle_ = nullptr;
  }
  return *this;
}

MqttClient::~MqttClient() {
  if (handle_) {
    mosquitto_loop_stop(handle_, /*force=*/true);
    mosquitto_destroy(handle_);
  }
}

std::optional<MqttClient> MqttClient::Open(const ConnectionOptions& options) {
  static std::once_flag lib_init_flag;
  std::call_once(lib_init_flag, [] { mosquitto_lib_init(); });

  auto connected = std::make_shared<std::atomic<bool>>(false);
  mosquitto* handle =
      mosquitto_new(options.client_id.c_str(), /*clean_session=*/true, connected.get());
  if (!handle) {
    return std::nullopt;
  }

  if (!options.username.empty()) {
    mosquitto_username_pw_set(handle, options.username.c_str(), options.password.c_str());
  }

  mosquitto_connect_callback_set(handle, &OnConnect);
  mosquitto_disconnect_callback_set(handle, &OnDisconnect);
  mosquitto_reconnect_delay_set(handle, /*reconnect_delay=*/1, /*reconnect_delay_max=*/30,
                                 /*reconnect_exponential_backoff=*/true);

  if (mosquitto_connect_async(handle, options.broker_host.c_str(), options.broker_port,
                               options.keepalive_seconds) != MOSQ_ERR_SUCCESS) {
    mosquitto_destroy(handle);
    return std::nullopt;
  }

  if (mosquitto_loop_start(handle) != MOSQ_ERR_SUCCESS) {
    mosquitto_destroy(handle);
    return std::nullopt;
  }

  return MqttClient(handle, connected);
}

bool MqttClient::Publish(const std::string& topic, const std::string& payload, int qos,
                          bool retain) {
  int rc = mosquitto_publish(handle_, /*mid=*/nullptr, topic.c_str(),
                              static_cast<int>(payload.size()), payload.data(), qos, retain);
  return rc == MOSQ_ERR_SUCCESS;
}

bool MqttClient::IsConnected() const { return connected_->load(); }

}  // namespace mqtt
```

- [ ] **Step 3: 改 `CMakeLists.txt`**

在 `enable_testing()` 之后、`add_library(cns_rpi_core ...)` 之前加：

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)
```

在 `add_executable(cns_rpi src/main.cpp)` 之前（`cns_rpi_core` 定义完之后）新增一个独立库目标，不并入 `cns_rpi_core`（这样 `test_mqtt_topic` 等只需要 `cns_rpi_core` 的测试不会被迫链接 libmosquitto）：

```cmake
# cns_rpi_mqtt 独立成库，不并入 cns_rpi_core：只有它需要链接真实的
# libmosquitto，这样 cns_rpi_core 的其他消费者（各 test_* target）
# 不会被迫在链接时也依赖 libmosquitto。
add_library(cns_rpi_mqtt src/mqtt/mqtt_client.cpp)
target_include_directories(cns_rpi_mqtt PUBLIC src)
target_link_libraries(cns_rpi_mqtt PUBLIC PkgConfig::MOSQUITTO)
```

- [ ] **Step 4: 只编译这个新库目标，验证能过**

```bash
cmake -S . -B build
cmake --build build --target cns_rpi_mqtt
```

Expected: 编译无警告，生成 `libcns_rpi_mqtt.a`。（这一步之后再跑一次完整 `cmake --build build` 确认没有把其他已有 target 编坏。）

```bash
cmake --build build
```

Expected: 全部 target（含 `cns_rpi`、各 `test_*`）编译通过。

- [ ] **Step 5: Commit**

```bash
git add src/mqtt/mqtt_client.hpp src/mqtt/mqtt_client.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: 新增mqtt/mqtt_client包装libmosquitto

RAII包装libmosquitto：Open()异步连接+起后台网络线程(断线自动重连交给
libmosquitto自己的mosquitto_reconnect_delay_set)，Publish()很薄只转发，
IsConnected()读回调维护的原子标志。独立成cns_rpi_mqtt库目标，不并入
cns_rpi_core，避免其他不需要MQTT的测试目标被迫链接libmosquitto。
不写自动化单测(真连外部broker，跟uart::MavlinkLink对真实串口同样处理)。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `main.cpp` 接入——身份就绪后连接、1Hz 发布

**前置条件**：本 Task 的人工验证步骤（Step 5）需要一个能连的 MQTT broker。本机自装：`sudo apt install mosquitto`，装完后用 `mosquitto` 命令直接起（默认监听 `127.0.0.1:1883`）。

**Files:**
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`（`cns_rpi` 可执行文件链接新增的 `cns_rpi_mqtt`）

**Interfaces:**
- Consumes: `mqtt::BuildTelemetryTopic`（Task 1）、`mqtt::ConnectionOptions`/`mqtt::MqttClient`（Task 2）、`payload::ToJson`（M4 已有）、`state_store.Snapshot().vendor_id`（M3c 已有，`std::optional<std::string>`）、`app_config->mqtt`（M2 已有的 `config::MqttConfig`，字段：`broker_host`/`broker_port`/`client_id`/`username`/`password`/`topic_prefix`/`qos`/`keepalive_seconds`）。
- Produces: 无（`main.cpp` 是组合根，不被其他文件依赖）。

本 Task 没有自动化单测（`main.cpp` 从来没有——`LogTelemetry`/`LogExtension`/`LogJsonPayload` 都没测，这次维持同样边界），验收标准是编译通过 + 现有 ctest 套件不受影响 + 人工验证步骤（Step 5）。

- [ ] **Step 1: 改 `main.cpp` 的 `#include` 和文件头注释**

把：

```cpp
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "common/mavlink.h"
#include "config/app_config.hpp"
#include "payload/json_serializer.hpp"
#include "protocol/extension_decoder.hpp"
#include "protocol/identity.hpp"
#include "protocol/telemetry_decoder.hpp"
#include "state/state_store.hpp"
#include "uart/mavlink_link.hpp"
```

改成：

```cpp
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include "common/mavlink.h"
#include "config/app_config.hpp"
#include "mqtt/mqtt_client.hpp"
#include "mqtt/topic.hpp"
#include "payload/json_serializer.hpp"
#include "protocol/extension_decoder.hpp"
#include "protocol/identity.hpp"
#include "protocol/telemetry_decoder.hpp"
#include "state/state_store.hpp"
#include "uart/mavlink_link.hpp"
```

文件头注释里的这句：

```cpp
 * 启动时读一次 RPi 本机序列号(V1 过渡期权威键)。不接 MQTT（M5 的事）。
```

改成：

```cpp
 * 启动时读一次 RPi 本机序列号(V1 过渡期权威键)。M5 阶段接入 MQTT 发布：
 * 身份就绪(state_store.vendor_id 有值)后才连接 broker，连上后按 1Hz 固定
 * 节奏把 payload::ToJson() 的结果发布到 {topic_prefix}/{vendor_id}/telemetry，
 * retain=true；断线重连交给 libmosquitto 自己处理，不做应用层重试。
```

- [ ] **Step 2: 新增 `kTelemetryPublishInterval` 常量**

把：

```cpp
constexpr std::uint8_t kSystemId = 1;
constexpr std::uint8_t kComponentId = MAV_COMP_ID_ONBOARD_COMPUTER;
constexpr auto kHeartbeatInterval = std::chrono::seconds(1);
```

改成：

```cpp
constexpr std::uint8_t kSystemId = 1;
constexpr std::uint8_t kComponentId = MAV_COMP_ID_ONBOARD_COMPUTER;
constexpr auto kHeartbeatInterval = std::chrono::seconds(1);
constexpr auto kTelemetryPublishInterval = std::chrono::seconds(1);
```

- [ ] **Step 3: 主循环外新增 MQTT 相关变量**

把：

```cpp
  state::StateStore state_store;
  if (auto serial = protocol::ReadRpiSerial()) {
    state_store.UpdateRpiSerial(*serial);
  }
  auto last_heartbeat = std::chrono::steady_clock::now();
  while (true) {
```

改成：

```cpp
  state::StateStore state_store;
  if (auto serial = protocol::ReadRpiSerial()) {
    state_store.UpdateRpiSerial(*serial);
  }
  auto last_heartbeat = std::chrono::steady_clock::now();
  std::optional<mqtt::MqttClient> mqtt_client;
  std::string telemetry_topic;
  auto last_telemetry_publish = std::chrono::steady_clock::now();
  while (true) {
```

- [ ] **Step 4: 主循环里新增连接判断 + 发布判断**

把：

```cpp
    auto now = std::chrono::steady_clock::now();
    if (now - last_heartbeat >= kHeartbeatInterval) {
      mavlink_message_t heartbeat = BuildHeartbeat();
      link->SendMessage(heartbeat);
      last_heartbeat = now;
    }
  }
}
```

改成：

```cpp
    auto now = std::chrono::steady_clock::now();
    if (now - last_heartbeat >= kHeartbeatInterval) {
      mavlink_message_t heartbeat = BuildHeartbeat();
      link->SendMessage(heartbeat);
      last_heartbeat = now;
    }

    if (!mqtt_client) {
      auto snapshot = state_store.Snapshot();
      if (snapshot.vendor_id) {
        telemetry_topic = mqtt::BuildTelemetryTopic(app_config->mqtt.topic_prefix, *snapshot.vendor_id);
        mqtt_client = mqtt::MqttClient::Open({
            .broker_host = app_config->mqtt.broker_host,
            .broker_port = app_config->mqtt.broker_port,
            .client_id = app_config->mqtt.client_id,
            .username = app_config->mqtt.username,
            .password = app_config->mqtt.password,
            .keepalive_seconds = app_config->mqtt.keepalive_seconds,
        });
        if (mqtt_client) {
          std::cout << "MQTT连接中: broker=" << app_config->mqtt.broker_host
                    << " topic=" << telemetry_topic << std::endl;
        } else {
          std::cerr << "MQTT客户端创建失败，下一轮重试" << std::endl;
        }
      }
    }

    if (mqtt_client && mqtt_client->IsConnected() &&
        now - last_telemetry_publish >= kTelemetryPublishInterval) {
      std::string json_str =
          payload::ToJson(state_store.Snapshot(), app_config->identity.school_name).dump();
      if (!mqtt_client->Publish(telemetry_topic, json_str, app_config->mqtt.qos, /*retain=*/true)) {
        std::cerr << "MQTT发布失败，下个节拍重试" << std::endl;
      }
      last_telemetry_publish = now;
    }
  }
}
```

- [ ] **Step 5: 改 `CMakeLists.txt`，`cns_rpi` 链接 `cns_rpi_mqtt`**

把：

```cmake
add_executable(cns_rpi src/main.cpp)
target_link_libraries(cns_rpi PRIVATE cns_rpi_core)
```

改成：

```cmake
add_executable(cns_rpi src/main.cpp)
target_link_libraries(cns_rpi PRIVATE cns_rpi_core cns_rpi_mqtt)
```

- [ ] **Step 6: 编译，跑现有全部 ctest 套件确认没有回归**

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: 编译无警告；全部 8 个测试套件（`mavlink_link`/`app_config`/`state_store`/`telemetry_decoder`/`extension_decoder`/`identity`/`json_serializer`/`mqtt_topic`）全部 PASS。

- [ ] **Step 7: 人工验证（本机自装 broker，自连自发自订阅）**

1. 起本地 broker（前置条件已装好 `mosquitto`）：

```bash
mosquitto -v
```

2. 另开一个终端订阅（`mosquitto-clients` 包提供 `mosquitto_sub`，如果没装：`sudo apt install mosquitto-clients`）：

```bash
mosquitto_sub -h 127.0.0.1 -t 'cns_rpi/+/telemetry' -v
```

3. 确认 `config/config.json` 的 `mqtt.broker_host` 指向 `127.0.0.1`（或者留着示例值改成 `127.0.0.1`）。

4. 第三个终端跑 `cns_rpi`（接 `tools/mavlink_sim/send_frames.py` 模拟身份帧和遥测，跟之前 M4 验证同样的 pty 桥接方式，或者接真机 STM32）。

5. 确认：
   - `cns_rpi` 收到身份帧（`OPEN_DRONE_ID_BASIC_ID`）后打出一行 `MQTT连接中: broker=... topic=...`；
   - `mosquitto_sub` 那个终端能收到 JSON 消息，topic 里的 `vendor_id` 段跟 JSON 内容里 `identity.vendor_id` 字段值一致；
   - 停掉再重新执行一次 `mosquitto_sub`（新订阅），立刻收到最后一条（验证 `retain=true` 生效，不用等下一个 1Hz 节拍）；
   - 手动 Ctrl+C 杀掉 `mosquitto -v` 进程再重新起一次，观察 `cns_rpi` 不崩溃、日志打出"MQTT连接断开"再"MQTT已连接"，`mosquitto_sub`恢复收到新消息（不需要重启 `cns_rpi` 本身）。

- [ ] **Step 8: Commit**

```bash
git add src/main.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: main.cpp接入MQTT发布,身份就绪后连接+1Hz固定节奏发布

身份就绪(state_store.vendor_id有值)前main循环不初始化MQTT，没有超时
兜底；连上后按1Hz节奏(不跟随MAVLink帧到达频率)把payload::ToJson()发布
到{topic_prefix}/{vendor_id}/telemetry，retain=true；断线交给libmosquitto
自动重连，不做应用层补发。已用本机mosquitto broker+mosquitto_sub人工
验证:连接/发布/retain/断线重连全部符合预期。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: 同步 `docs/V1设计文档.md` §10 里程碑状态

**Files:**
- Modify: `docs/V1设计文档.md`

**Interfaces:** 无（纯文档）。

- [ ] **Step 1: 更新 §10 M5 那一行**

找到：

```markdown
- **M5 MQTT 发布**：libmosquitto client 连接/重连/发布，先连开发机 Mosquitto 自行订阅验证；遥测 topic 命名方案定稿
```

改成：

```markdown
- **M5 MQTT 发布**：libmosquitto client 连接/重连/发布，先连开发机 Mosquitto 自行订阅验证；遥测 topic 命名方案定稿——已实现，见 `docs/superpowers/plans/2026-07-09-m5-mqtt-publish.md`；topic 定稿为 `{topic_prefix}/{vendor_id}/telemetry`，身份就绪前不连 MQTT（无超时兜底），1Hz 固定节奏发布 + retain
```

- [ ] **Step 2: 更新 §11 "Topic 命名方案最终字符串格式"那条开放问题**

找到：

```markdown
- Topic 命名方案最终字符串格式——待 M3c（身份帧解码）完成、能拿到厂商唯一产品识别码后定稿，不阻塞现有里程碑顺序；M5 实现 MQTT 收发时需要显式处理"等身份就绪再连 MQTT"这个启动依赖（详见第 2、9 节），具体的等待/超时/重试策略留到 M5 实现阶段设计
```

改成：

```markdown
- ~~Topic 命名方案最终字符串格式~~——已在 M5 定稿：`{topic_prefix}/{vendor_id}/telemetry`；"等身份就绪再连 MQTT"这个启动依赖也已在 M5 实现，策略是一直等、不设超时兜底，详见 `docs/superpowers/specs/2026-07-09-m5-mqtt-publish-design.md`
```

- [ ] **Step 3: Commit**

```bash
git add docs/V1设计文档.md
git commit -m "$(cat <<'EOF'
docs: M5落地后同步V1设计文档里程碑状态

topic命名方案定稿{topic_prefix}/{vendor_id}/telemetry，身份就绪等待策略
(一直等不设超时)也已实现，§10/§11对应条目标记已完成。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review 记录

- **Spec 覆盖检查**：设计文档 §2（topic 命名）→ Task 1；§3（身份等待策略）→ Task 3 Step 4；§4（1Hz 发布节奏）→ Task 3 Step 2/4；§5（组件设计）→ Task 1/2；§6（main.cpp 改动）→ Task 3；§7（错误处理）→ Task 2/3 代码里已体现（Open 失败重试、断线自动重连、Publish 失败下个节拍重试）；§8（测试计划）→ Task 1 单测 + Task 2/3 明确不写自动化单测；§9（人工验证步骤）→ Task 3 Step 7；§10（CMake 改动）→ Task 2 Step 3、Task 3 Step 5。全部覆盖，没有遗漏。
- **占位符扫描**：没有 TBD/TODO/"补充完整错误处理"这类占位描述，每个 Step 都是完整代码。
- **类型一致性检查**：`mqtt::ConnectionOptions` 在 Task 2 Step 1 声明的 6 个字段（`broker_host`/`broker_port`/`client_id`/`username`/`password`/`keepalive_seconds`）跟 Task 3 Step 4 里 `MqttClient::Open({...})` 调用时用的指定初始化字段完全一致，顺序也一致（designated initializer 要求顺序匹配）。`mqtt::BuildTelemetryTopic` 的签名在 Task 1/Task 3 两处一致。`payload::ToJson` 签名沿用 M4 已有的 `(const state::TelemetryState&, const std::string&) -> nlohmann::json`，未改动。
- **范围检查**：4 个 Task 都在 M5 范围内，没有涉及 M6（命令下行/订阅）的内容；`config.json` schema 未改动，符合 Global Constraints。
