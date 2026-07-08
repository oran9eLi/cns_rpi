# M4 JSON Payload 构造 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `state::TelemetryState`（M3a/M3b/M3c 已解码好的原始 MAVLink 数据）转成一份人类可读、单位换算过的 JSON，每收到一帧就重新序列化打印一次，供人工核对字段可读性（M4 里程碑，`docs/V1设计文档.md` §10）。

**Architecture:** 新增 `payload::ToJson(state, school_name)` 一个纯函数，只依赖 `state::TelemetryState`（不依赖 `config::AppConfig` 整个结构体，也不依赖 `protocol/` 解码层），按 `docs/superpowers/specs/2026-07-07-m4-json-payload-design.md`（2026-07-08 第二次修订版）里定的字段清单/换算公式/省略规则组装 `nlohmann::json`。落地前有一个前置改动：`state_store.battery_status` 从单一坑位改成按 `BATTERY_STATUS.id` 区分存储的数组，因为固件即将改成用两条不同 `id` 的 `BATTERY_STATUS` 表示电池1/电池2。

**Tech Stack:** C++23，`nlohmann::json`（已经是项目依赖），`doctest`（已有的测试框架）。

## Global Constraints

- `state_store` 层"存原样不做单位换算"的原则不变——本计划所有单位换算只发生在 `payload/json_serializer.cpp` 里，不改 `state/`、`protocol/` 的解码逻辑（除了 Task 1 的 `battery_status` 按 id 存储这一个前置例外，那也不涉及单位换算，只是存储结构调整）。
- 未收到过的字段省略 JSON key，不输出 `null`；`null` 只用于"消息收到过，但该字段命中协议定义的哨兵值"这一种情况——具体清单见每个 Task 里的哨兵值处理。
- key 一律 snake_case。
- 标准 MAVLink/ODID 官方枚举保持原始数字，不维护枚举名映射表；仅本项目自定义的模块状态枚举（含 `LORASTAT.link_state` 复用的同一枚举）转字符串。
- 这套实训箱固定不动、不飞、不编队：`global_position.vx/vy/vz/relative_alt`、`gps.vel/cog/yaw/vel_acc/hdg_acc`、`drone_id.location.speed_horizontal/speed_vertical/direction/height/height_reference/speed_accuracy`、`drone_id.system.area_ceiling/area_floor/area_count/area_radius` 一律不输出到 JSON（`state_store` 解码层不受影响，仍然原样存储官方结构体全部字段）。
- 每个任务完成后运行对应测试，全绿才算完成；每个任务结束提交一次（`git commit`），不要攒到最后一次性提交。
- 编译命令统一用：`cmake --build build`（假设 `build/` 目录已经用 `cmake -S . -B build` 配置过；如果还没有，先运行一次 `cmake -S . -B build`）。

---

## Task 1: 前置改动——`battery_status` 按 `id` 区分存储

固件即将改成发两条不同 `id` 的 `BATTERY_STATUS`（`id=0`=电池1，`id=1`=电池2）。现在 `state_store.hpp` 的 `battery_status` 是单一坑位，`UpdateBatteryStatus` 收到就直接覆盖，不看 `id`——两条不同 `id` 的消息会互相覆盖丢数据。这个任务把它改成按 `id` 存的定长数组。

**Files:**
- Modify: `src/state/state_store.hpp:116`（`battery_status` 字段类型）、`:159`（`UpdateBatteryStatus` 声明）
- Modify: `src/state/state_store.cpp:35-38`（`UpdateBatteryStatus` 实现）
- Modify: `src/protocol/telemetry_decoder.cpp:42-47`（`MAVLINK_MSG_ID_BATTERY_STATUS` 分支）
- Modify: `src/main.cpp:79-85`（`LogTelemetry` 里打印 `BATTERY_STATUS` 的分支）
- Modify: `tests/test_state_store.cpp:19`、`:87-90`（改用数组下标）
- Modify: `tests/test_telemetry_decoder.cpp:116-137`（`BATTERY_STATUS解码写入store` 测试改用数组下标，并新增按 id 独立存储的测试）

**Interfaces:**
- Produces: `state::kBatteryCount`（`constexpr std::size_t = 2`）；`state::TelemetryState::battery_status` 类型变为 `std::array<std::optional<mavlink_battery_status_t>, kBatteryCount>`；`StateStore::UpdateBatteryStatus(std::uint8_t id, const mavlink_battery_status_t& value)`（新增 `id` 参数，`id >= kBatteryCount` 时静默丢弃，不写入、不报错，同现有解码层"不认识就丢弃"的一贯风格）。

- [ ] **Step 1: 改 `state_store.hpp`**

在 `src/state/state_store.hpp` 里，`kModuleCount` 常量下方新增：

```cpp
/// BATTERY_STATUS.id 目前只处理电池1(id=0)/电池2(id=1)两块，超出范围的id丢弃。
constexpr std::size_t kBatteryCount = 2;
```

把 `TelemetryState` 里这一行：

```cpp
  std::optional<mavlink_battery_status_t> battery_status;
```

改成：

```cpp
  /// 按 BATTERY_STATUS.id 区分存储：下标0是电池1(id=0)，下标1是电池2(id=1)。
  /// 固件用不同id区分两块电池，同一坑位直接覆盖会导致两块电池数据互相打架。
  std::array<std::optional<mavlink_battery_status_t>, kBatteryCount> battery_status;
```

把 `StateStore` 类里这一行：

```cpp
  void UpdateBatteryStatus(const mavlink_battery_status_t& value);
```

改成：

```cpp
  /// id超出kBatteryCount范围时静默丢弃，不写入、不报错(同解码层一贯的"不认识就丢弃"风格)。
  void UpdateBatteryStatus(std::uint8_t id, const mavlink_battery_status_t& value);
```

- [ ] **Step 2: 改 `state_store.cpp`**

把 `state_store.cpp` 里的：

```cpp
void StateStore::UpdateBatteryStatus(const mavlink_battery_status_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.battery_status = value;
}
```

改成：

```cpp
void StateStore::UpdateBatteryStatus(std::uint8_t id, const mavlink_battery_status_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (id < kBatteryCount) {
    state_.battery_status[id] = value;
  }
}
```

- [ ] **Step 3: 改 `telemetry_decoder.cpp`**

把 `telemetry_decoder.cpp` 里的：

```cpp
    case MAVLINK_MSG_ID_BATTERY_STATUS: {
      mavlink_battery_status_t decoded{};
      mavlink_msg_battery_status_decode(&msg, &decoded);
      store.UpdateBatteryStatus(decoded);
      return true;
    }
```

改成：

```cpp
    case MAVLINK_MSG_ID_BATTERY_STATUS: {
      mavlink_battery_status_t decoded{};
      mavlink_msg_battery_status_decode(&msg, &decoded);
      store.UpdateBatteryStatus(decoded.id, decoded);
      return true;
    }
```

- [ ] **Step 4: 改现有测试，让它们先能编译（这一步不是新增测试，是修复因类型变化而编译失败的旧测试）**

`tests/test_state_store.cpp` 第 19 行：

```cpp
  CHECK_FALSE(snapshot.battery_status.has_value());
```

改成：

```cpp
  CHECK_FALSE(snapshot.battery_status[0].has_value());
  CHECK_FALSE(snapshot.battery_status[1].has_value());
```

`tests/test_telemetry_decoder.cpp` 第 116-137 行的 `TEST_CASE("BATTERY_STATUS解码写入store")` 里：

```cpp
  REQUIRE(snapshot.battery_status.has_value());
  // mavlink_battery_status_t 是 packed 结构体，voltages[] 数组元素是未对齐的多字节
  // 字段，同上：static_cast 规避 GCC 14 下 CHECK 宏的引用绑定问题。
  CHECK(static_cast<std::uint16_t>(snapshot.battery_status->voltages[0]) == 4200);
  CHECK(snapshot.battery_status->battery_remaining == 80);
```

改成：

```cpp
  REQUIRE(snapshot.battery_status[0].has_value());
  // mavlink_battery_status_t 是 packed 结构体，voltages[] 数组元素是未对齐的多字节
  // 字段，同上：static_cast 规避 GCC 14 下 CHECK 宏的引用绑定问题。
  CHECK(static_cast<std::uint16_t>(snapshot.battery_status[0]->voltages[0]) == 4200);
  CHECK(snapshot.battery_status[0]->battery_remaining == 80);
```

（这个 `mavlink_msg_battery_status_pack` 调用本身 `id=0`，不用改。）

- [ ] **Step 5: 新增测试——验证按 id 独立存储、互不覆盖**

在 `tests/test_telemetry_decoder.cpp` 末尾（`不认识的消息类型被安静忽略...` 这个 `TEST_CASE` 之前或之后均可）新增：

```cpp
TEST_CASE("两条不同id的BATTERY_STATUS各自独立存储，互不覆盖") {
  state::StateStore store;
  std::uint16_t voltages1[10] = {4200, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  std::uint16_t voltages_ext[4] = {0, 0, 0, 0};
  mavlink_message_t msg1{};
  mavlink_msg_battery_status_pack(kSystemId, kComponentId, &msg1, /*id=*/0,
                                  /*battery_function=*/0, /*type=*/0, /*temperature=*/2500,
                                  voltages1, /*current_battery=*/150, /*current_consumed=*/500,
                                  /*energy_consumed=*/1000, /*battery_remaining=*/80,
                                  /*time_remaining=*/3600, /*charge_state=*/0, voltages_ext,
                                  /*mode=*/0, /*fault_bitmask=*/0);

  std::uint16_t voltages2[10] = {4100, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  mavlink_message_t msg2{};
  mavlink_msg_battery_status_pack(kSystemId, kComponentId, &msg2, /*id=*/1,
                                  /*battery_function=*/0, /*type=*/0, /*temperature=*/2400,
                                  voltages2, /*current_battery=*/90, /*current_consumed=*/300,
                                  /*energy_consumed=*/800, /*battery_remaining=*/60,
                                  /*time_remaining=*/2800, /*charge_state=*/0, voltages_ext,
                                  /*mode=*/0, /*fault_bitmask=*/0);

  protocol::DecodeAndStore(msg1, store);
  protocol::DecodeAndStore(msg2, store);
  auto snapshot = store.Snapshot();

  REQUIRE(snapshot.battery_status[0].has_value());
  REQUIRE(snapshot.battery_status[1].has_value());
  CHECK(static_cast<std::uint16_t>(snapshot.battery_status[0]->voltages[0]) == 4200);
  CHECK(static_cast<std::uint16_t>(snapshot.battery_status[1]->voltages[0]) == 4100);
  CHECK(snapshot.battery_status[0]->battery_remaining == 80);
  CHECK(snapshot.battery_status[1]->battery_remaining == 60);
}
```

- [ ] **Step 6: 改 `main.cpp` 打印分支**

把 `main.cpp` 里 `LogTelemetry` 函数中的：

```cpp
    case MAVLINK_MSG_ID_BATTERY_STATUS:
      if (snapshot.battery_status) {
        std::cout << "BATTERY_STATUS: voltages[0]=" << snapshot.battery_status->voltages[0]
                  << " battery_remaining="
                  << static_cast<int>(snapshot.battery_status->battery_remaining) << std::endl;
      }
      break;
```

改成：

```cpp
    case MAVLINK_MSG_ID_BATTERY_STATUS:
      for (std::size_t i = 0; i < snapshot.battery_status.size(); ++i) {
        if (snapshot.battery_status[i]) {
          std::cout << "BATTERY_STATUS[" << i
                    << "]: voltages[0]=" << snapshot.battery_status[i]->voltages[0]
                    << " battery_remaining="
                    << static_cast<int>(snapshot.battery_status[i]->battery_remaining)
                    << std::endl;
        }
      }
      break;
```

- [ ] **Step 7: 编译并运行测试，确认全绿**

```bash
cmake --build build
ctest --test-dir build --output-on-failure -R "state_store|telemetry_decoder"
```

Expected: 两个测试套件全部 PASS（`state_store` 里新增的下标断言、`telemetry_decoder` 里改过的测试 + 新增的 id 独立存储测试）。

- [ ] **Step 8: Commit**

```bash
git add src/state/state_store.hpp src/state/state_store.cpp src/protocol/telemetry_decoder.cpp src/main.cpp tests/test_state_store.cpp tests/test_telemetry_decoder.cpp
git commit -m "$(cat <<'EOF'
refactor: battery_status按BATTERY_STATUS.id区分存储

固件即将改成用id=0/id=1两条BATTERY_STATUS表示电池1/电池2，
原来单一坑位会被后到的一条覆盖，两块电池数据互相打架。改成
按id索引的数组，为M4 JSON payload设计里电池2改走官方通道做准备。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `config::IdentityConfig`——`school_name` 配置读取

**Files:**
- Modify: `src/config/app_config.hpp:44-53`（新增 `IdentityConfig`，`AppConfig` 加字段）
- Modify: `src/config/app_config.cpp:31-46`（`LoadAppConfig` 解析 `identity.school_name`）
- Modify: `config/config.example.json`（新增 `identity` 块）
- Modify: `tests/test_app_config.cpp`（现有测试的 JSON fixture 补上 `identity` 块，否则会因缺字段变成 `kMissingField` 失败；新增一条 identity 专属测试）

**Interfaces:**
- Produces: `config::IdentityConfig { std::string school_name; }`；`config::AppConfig::identity`（新增字段）。

- [ ] **Step 1: 改 `app_config.hpp`**

在 `LoggingConfig` 定义之后、`AppConfig` 定义之前新增：

```cpp
struct IdentityConfig {
  std::string school_name;
};
```

把 `AppConfig` 改成：

```cpp
struct AppConfig {
  SerialConfig serial;
  MqttConfig mqtt;
  LoggingConfig logging;
  IdentityConfig identity;
};
```

- [ ] **Step 2: 改 `app_config.cpp`**

在 `LoadAppConfig` 里，`logging` 解析代码块之后新增：

```cpp
    const auto& identity = root.at("identity");
    cfg.identity.school_name = identity.at("school_name").get<std::string>();
```

（这一段跟 `logging`/`mqtt` 一样放在同一个 `try` 块里，缺字段/类型不对时复用同一套 `ConfigError::kMissingField`/`kInvalidValue` 处理，不需要额外的 `catch`。）

- [ ] **Step 3: 改 `config/config.example.json`**

在 `"logging"` 块后面新增（注意补上前一个块末尾的逗号）：

```json
  "identity": {
    "school_name": "NNUTC"
  }
```

- [ ] **Step 4: 改现有测试 fixture，让它们先能通过（这一步是修复，不是新增）**

`tests/test_app_config.cpp` 里所有内联的 JSON fixture 字符串（`WriteTempConfig(R"({...})")` 调用），只要包含完整的 `serial`+`mqtt`+`logging` 三段、且没有刻意测"缺字段"的，都要在 `"logging": {...}` 后面加上：

```json
    ,"identity": {"school_name": "NNUTC"}
```

具体来说：

- `TEST_CASE("完整合法配置文件能正确解析出serial/mqtt/logging字段")`（第 21-38 行）：fixture 里加 `identity` 块，并且在 `CHECK` 部分末尾加一行 `CHECK(result->identity.school_name == "NNUTC");`。
- `TEST_CASE("字段存在但类型不对时返回kInvalidValue")`（第 65-77 行）：fixture 里也加上合法的 `identity` 块（这条测试本身测的是 `serial.baud` 类型错，不测 identity，所以 identity 给合法值就行，不然会先命中 `kMissingField` 而不是想测的 `kInvalidValue`）。

`TEST_CASE("配置文件不存在时返回kFileNotFound")`、`TEST_CASE("JSON格式损坏时返回kParseError")`、`TEST_CASE("缺少必需字段时返回kMissingField")`、`TEST_CASE("serial有效但mqtt整节缺失时返回kMissingField")` 这四条测的就是"缺字段/格式错"本身，不用改。

- [ ] **Step 5: 新增测试——`identity` 整节缺失时返回 `kMissingField`**

在 `tests/test_app_config.cpp` 末尾新增：

```cpp
TEST_CASE("serial/mqtt/logging都有效但identity整节缺失时返回kMissingField") {
  auto path = WriteTempConfig(R"({
    "serial": {"device": "/dev/ttyUSB0", "baud": 115200},
    "mqtt": {"broker_host": "192.168.1.100", "broker_port": 1883, "client_id": "cns-rpi",
             "username": "", "password": "", "topic_prefix": "cns_rpi", "qos": 1, "keepalive_seconds": 60},
    "logging": {"level": "info", "file": ""}
  })");

  auto result = config::LoadAppConfig(path);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kMissingField);
}
```

- [ ] **Step 6: 编译并运行测试**

```bash
cmake --build build
ctest --test-dir build --output-on-failure -R app_config
```

Expected: 全部 PASS。

- [ ] **Step 7: Commit**

```bash
git add src/config/app_config.hpp src/config/app_config.cpp config/config.example.json tests/test_app_config.cpp
git commit -m "$(cat <<'EOF'
feat: AppConfig新增identity.school_name配置项

M4 JSON payload的identity块需要学校名称，这是本机静态配置，
不是STM32解码出来的数据，所以放进config.json而不是state_store。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `payload/json_serializer` 骨架 + `identity` 块

新建 `payload/` 模块，先把最小可用版本跑通：空状态下只输出 `identity.school_name`，其余字段没有；再补齐 `identity` 里另外三个可选字段。

**Files:**
- Create: `src/payload/json_serializer.hpp`
- Create: `src/payload/json_serializer.cpp`
- Create: `tests/test_json_serializer.cpp`
- Modify: `CMakeLists.txt`（新增 `payload/json_serializer.cpp` 到 `cns_rpi_core`，新增 `test_json_serializer` target）

**Interfaces:**
- Produces: `payload::ToJson(const state::TelemetryState& state, const std::string& school_name) -> nlohmann::json`（本文件后续所有 Task 都往这个函数里加逻辑，函数名/签名固定不变）。

- [ ] **Step 1: 写 `json_serializer.hpp`**

```cpp
#pragma once

/**
 * @file json_serializer.hpp
 * @brief 把 state::TelemetryState 转成人类可读、单位换算过的 JSON。
 *
 * @details
 * 只依赖 state::TelemetryState，不依赖 config::AppConfig 整个结构体（school_name
 * 作为独立参数传入），也不依赖 protocol/ 解码层——保持"解码与消费者分离"的既定
 * 架构原则。字段清单/换算公式/省略规则见
 * docs/superpowers/specs/2026-07-07-m4-json-payload-design.md。
 * 依赖边界：只依赖 state/state_store.hpp 和 nlohmann/json，不包含 uart/、
 * protocol/、config/、mqtt/ 等其他模块头文件。
 */

#include <string>

#include <nlohmann/json.hpp>

#include "state/state_store.hpp"

namespace payload {

/**
 * @brief 把一份遥测快照转成 JSON。
 * @param state 当前的完整遥测快照（state::StateStore::Snapshot() 的返回值）。
 * @param school_name 本机静态配置的学校名称（来自 config.json，不是 STM32 解码出来的）。
 * @return 按设计文档规则组装好的 JSON；未收到过的字段对应的 key 不存在（不输出 null）。
 */
nlohmann::json ToJson(const state::TelemetryState& state, const std::string& school_name);

}  // namespace payload
```

- [ ] **Step 2: 写 `json_serializer.cpp`（骨架 + identity 全部 4 个字段）**

```cpp
/**
 * @file json_serializer.cpp
 * @brief json_serializer.hpp 的实现。
 */

#include "payload/json_serializer.hpp"

namespace payload {

namespace {

nlohmann::json BuildIdentity(const state::TelemetryState& state, const std::string& school_name) {
  nlohmann::json identity;
  if (state.vendor_id) {
    identity["vendor_id"] = *state.vendor_id;
  }
  if (state.dcdw_label) {
    identity["dcdw_label"] = *state.dcdw_label;
  }
  if (state.rpi_serial) {
    identity["rpi_serial"] = *state.rpi_serial;
  }
  identity["school_name"] = school_name;
  return identity;
}

}  // namespace

nlohmann::json ToJson(const state::TelemetryState& state, const std::string& school_name) {
  nlohmann::json out;
  out["identity"] = BuildIdentity(state, school_name);
  return out;
}

}  // namespace payload
```

- [ ] **Step 3: 写 `tests/test_json_serializer.cpp`（先写会失败的测试——这一步测试其实会直接通过，因为 Step 2 已经写好了实现；后续每个 Task 都是先写测试再扩展 `ToJson`，这个 Task 例外，因为它本身就是骨架搭建）**

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "payload/json_serializer.hpp"

TEST_CASE("空TelemetryState只输出identity.school_name其余顶层key都不存在") {
  state::TelemetryState state{};

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("identity"));
  CHECK(json["identity"]["school_name"] == "NNUTC");
  CHECK_FALSE(json["identity"].contains("vendor_id"));
  CHECK_FALSE(json["identity"].contains("dcdw_label"));
  CHECK_FALSE(json["identity"].contains("rpi_serial"));
  CHECK_FALSE(json.contains("telemetry"));
  CHECK_FALSE(json.contains("modules"));
  CHECK_FALSE(json.contains("alarms"));
  CHECK_FALSE(json.contains("logs"));
  CHECK_FALSE(json.contains("drone_id"));
}

TEST_CASE("identity三个可选字段各自独立按需省略") {
  state::TelemetryState state{};
  state.vendor_id = "DCDWCNS1ABCDEFGHIJKL";

  auto json = payload::ToJson(state, "NNUTC");

  CHECK(json["identity"]["vendor_id"] == "DCDWCNS1ABCDEFGHIJKL");
  CHECK_FALSE(json["identity"].contains("dcdw_label"));
  CHECK_FALSE(json["identity"].contains("rpi_serial"));

  state.dcdw_label = "DCDW-007";
  state.rpi_serial = "100000001234abcd";
  auto json2 = payload::ToJson(state, "NNUTC");

  CHECK(json2["identity"]["dcdw_label"] == "DCDW-007");
  CHECK(json2["identity"]["rpi_serial"] == "100000001234abcd");
}
```

- [ ] **Step 4: 改 `CMakeLists.txt`**

在 `add_library(cns_rpi_core ...)` 的源文件列表里加一行：

```cmake
    src/payload/json_serializer.cpp
```

（放在 `src/protocol/identity.cpp` 后面即可。）

在文件末尾（`test_identity` 那段之后）新增：

```cmake
add_executable(test_json_serializer tests/test_json_serializer.cpp)
target_link_libraries(test_json_serializer PRIVATE cns_rpi_core)
add_test(NAME json_serializer COMMAND test_json_serializer)
```

- [ ] **Step 5: 编译并运行测试**

```bash
cmake --build build
ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: 两条 `TEST_CASE` 全部 PASS。

- [ ] **Step 6: Commit**

```bash
git add src/payload/json_serializer.hpp src/payload/json_serializer.cpp tests/test_json_serializer.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: 新增payload/json_serializer骨架和identity块

M4第一步：搭好payload::ToJson()的骨架、接入CMake构建，
先落地identity块(vendor_id/dcdw_label/rpi_serial按需省略，
school_name恒定存在)。后续Task在同一个函数里陆续补齐
telemetry/modules/alarms/logs/drone_id。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: `telemetry.heartbeat` + `telemetry.attitude`

从这个 Task 开始，每个 Task 都是：在 `json_serializer.cpp` 里新增一个 `Add___(nlohmann::json& telemetry, const state::TelemetryState& state)` 函数，并在 `ToJson()` 里补一行调用。`telemetry` 这个 JSON 对象只有在非空时才会被挂到顶层 `out["telemetry"]` 上——这个"顶层是否存在"的逻辑本身在最后一个 Task（Task 14）里统一处理，中间几个 Task 先把 `telemetry` 当一个已经存在的局部变量来写。

**Files:**
- Modify: `src/payload/json_serializer.cpp`
- Modify: `tests/test_json_serializer.cpp`

**Interfaces:**
- Consumes: `state::TelemetryState::heartbeat`(`std::optional<mavlink_heartbeat_t>`)、`::attitude`(`std::optional<mavlink_attitude_t>`)。
- Produces: `AddHeartbeat(nlohmann::json&, const state::TelemetryState&)`、`AddAttitude(nlohmann::json&, const state::TelemetryState&)`——之后的 Task 会新增同类型的 `Add*` 函数，都是这个签名。

- [ ] **Step 1: 写测试（先写，此时还没有 `telemetry` key，测试会失败）**

在 `tests/test_json_serializer.cpp` 末尾新增：

```cpp
TEST_CASE("heartbeat字段按原始数字透传,未收到时telemetry.heartbeat不存在") {
  state::TelemetryState state{};
  mavlink_heartbeat_t hb{};
  hb.custom_mode = 0;
  hb.type = 2;
  hb.autopilot = 12;
  hb.base_mode = 81;
  hb.system_status = 4;
  hb.mavlink_version = 3;
  state.heartbeat = hb;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("telemetry"));
  REQUIRE(json["telemetry"].contains("heartbeat"));
  CHECK(json["telemetry"]["heartbeat"]["type"] == 2);
  CHECK(json["telemetry"]["heartbeat"]["autopilot"] == 12);
  CHECK(json["telemetry"]["heartbeat"]["base_mode"] == 81);
  CHECK(json["telemetry"]["heartbeat"]["system_status"] == 4);
  CHECK(json["telemetry"]["heartbeat"]["mavlink_version"] == 3);
  CHECK_FALSE(json["telemetry"].contains("attitude"));
}

TEST_CASE("attitude弧度转角度") {
  state::TelemetryState state{};
  mavlink_attitude_t att{};
  att.time_boot_ms = 123456;
  att.roll = 1.0F;
  att.pitch = -0.5F;
  att.yaw = 0.0F;
  att.rollspeed = 0.1F;
  att.pitchspeed = -0.1F;
  att.yawspeed = 0.0F;
  state.attitude = att;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json["telemetry"].contains("attitude"));
  CHECK(json["telemetry"]["attitude"]["time_boot_ms"] == 123456);
  CHECK(json["telemetry"]["attitude"]["roll"].get<double>() == doctest::Approx(57.29578));
  CHECK(json["telemetry"]["attitude"]["pitch"].get<double>() == doctest::Approx(-28.64789));
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build build
ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: FAIL（`json.contains("telemetry")` 为 false，因为 `ToJson()` 现在还没有写 `telemetry` key）。

- [ ] **Step 3: 实现**

在 `json_serializer.cpp` 顶部加 `#include <numbers>`；在 `BuildIdentity` 后面新增：

```cpp
constexpr double kRadToDeg = 180.0 / std::numbers::pi;

void AddHeartbeat(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.heartbeat) {
    return;
  }
  const auto& hb = *state.heartbeat;
  telemetry["heartbeat"] = {
      {"custom_mode", hb.custom_mode}, {"type", hb.type},
      {"autopilot", hb.autopilot},     {"base_mode", hb.base_mode},
      {"system_status", hb.system_status}, {"mavlink_version", hb.mavlink_version},
  };
}

void AddAttitude(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.attitude) {
    return;
  }
  const auto& att = *state.attitude;
  telemetry["attitude"] = {
      {"time_boot_ms", att.time_boot_ms},
      {"roll", att.roll * kRadToDeg},
      {"pitch", att.pitch * kRadToDeg},
      {"yaw", att.yaw * kRadToDeg},
      {"rollspeed", att.rollspeed * kRadToDeg},
      {"pitchspeed", att.pitchspeed * kRadToDeg},
      {"yawspeed", att.yawspeed * kRadToDeg},
  };
}
```

把 `ToJson()` 改成：

```cpp
nlohmann::json ToJson(const state::TelemetryState& state, const std::string& school_name) {
  nlohmann::json out;
  out["identity"] = BuildIdentity(state, school_name);

  nlohmann::json telemetry = nlohmann::json::object();
  AddHeartbeat(telemetry, state);
  AddAttitude(telemetry, state);
  if (!telemetry.empty()) {
    out["telemetry"] = std::move(telemetry);
  }

  return out;
}
```

- [ ] **Step 4: 运行测试确认通过**

```bash
cmake --build build
ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: 全部 PASS。

- [ ] **Step 5: Commit**

```bash
git add src/payload/json_serializer.cpp tests/test_json_serializer.cpp
git commit -m "$(cat <<'EOF'
feat: json_serializer补充telemetry.heartbeat/attitude

heartbeat原始数字透传(官方枚举不转字符串)；attitude弧度转角度。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: `telemetry.gps` + `telemetry.global_position`

**Files:**
- Modify: `src/payload/json_serializer.cpp`
- Modify: `tests/test_json_serializer.cpp`

**Interfaces:**
- Consumes: `state::TelemetryState::gps_raw_int`、`::global_position_int`。
- Produces: `AddGps`、`AddGlobalPosition`（同 `Add*(nlohmann::json&, const state::TelemetryState&)` 签名）。

- [ ] **Step 1: 写测试**

```cpp
TEST_CASE("gps字段换算,eph/epv命中哨兵值时输出null,vel/cog/yaw等字段不输出") {
  state::TelemetryState state{};
  mavlink_gps_raw_int_t gps{};
  gps.time_usec = 1720000000000000ULL;
  gps.lat = 399042000;
  gps.lon = 1164074000;
  gps.alt = 43500;
  gps.eph = 65535;  // UINT16_MAX -> null
  gps.epv = 150;
  gps.vel = 500;
  gps.cog = 9000;
  gps.fix_type = 3;
  gps.satellites_visible = 14;
  gps.alt_ellipsoid = 21200;
  gps.h_acc = 1100;
  gps.v_acc = 1800;
  gps.vel_acc = 300;
  gps.hdg_acc = 500;
  gps.yaw = 0;
  state.gps_raw_int = gps;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["telemetry"]["gps"];

  CHECK(out["time_usec"] == 1720000000000000ULL);
  CHECK(out["lat"].get<double>() == doctest::Approx(39.9042));
  CHECK(out["lon"].get<double>() == doctest::Approx(116.4074));
  CHECK(out["alt"].get<double>() == doctest::Approx(43.5));
  CHECK(out["alt_ellipsoid"].get<double>() == doctest::Approx(21.2));
  CHECK(out["eph"].is_null());
  CHECK(out["epv"] == 150);
  CHECK(out["fix_type"] == 3);
  CHECK(out["satellites_visible"] == 14);
  CHECK(out["h_acc"].get<double>() == doctest::Approx(1.1));
  CHECK(out["v_acc"].get<double>() == doctest::Approx(1.8));
  CHECK_FALSE(out.contains("vel"));
  CHECK_FALSE(out.contains("cog"));
  CHECK_FALSE(out.contains("yaw"));
  CHECK_FALSE(out.contains("vel_acc"));
  CHECK_FALSE(out.contains("hdg_acc"));
}

TEST_CASE("global_position字段换算,vx/vy/vz/relative_alt不输出") {
  state::TelemetryState state{};
  mavlink_global_position_int_t pos{};
  pos.time_boot_ms = 123456;
  pos.lat = 399042000;
  pos.lon = 1164074000;
  pos.alt = 43500;
  pos.relative_alt = 10000;
  pos.vx = 100;
  pos.vy = 200;
  pos.vz = -50;
  pos.hdg = 8750;
  state.global_position_int = pos;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["telemetry"]["global_position"];

  CHECK(out["time_boot_ms"] == 123456);
  CHECK(out["lat"].get<double>() == doctest::Approx(39.9042));
  CHECK(out["alt"].get<double>() == doctest::Approx(43.5));
  CHECK(out["hdg"].get<double>() == doctest::Approx(87.5));
  CHECK_FALSE(out.contains("vx"));
  CHECK_FALSE(out.contains("vy"));
  CHECK_FALSE(out.contains("vz"));
  CHECK_FALSE(out.contains("relative_alt"));
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build build
ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: FAIL（`telemetry.gps`/`telemetry.global_position` 都不存在）。

- [ ] **Step 3: 实现**

在 `AddAttitude` 后面新增：

```cpp
void AddGps(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.gps_raw_int) {
    return;
  }
  const auto& gps = *state.gps_raw_int;
  nlohmann::json out;
  out["time_usec"] = gps.time_usec;
  out["lat"] = static_cast<double>(gps.lat) / 1e7;
  out["lon"] = static_cast<double>(gps.lon) / 1e7;
  out["alt"] = static_cast<double>(gps.alt) / 1000.0;
  out["alt_ellipsoid"] = static_cast<double>(gps.alt_ellipsoid) / 1000.0;
  out["eph"] = (gps.eph == UINT16_MAX) ? nlohmann::json(nullptr) : nlohmann::json(gps.eph);
  out["epv"] = (gps.epv == UINT16_MAX) ? nlohmann::json(nullptr) : nlohmann::json(gps.epv);
  out["fix_type"] = gps.fix_type;
  out["satellites_visible"] = gps.satellites_visible;
  out["h_acc"] = static_cast<double>(gps.h_acc) / 1000.0;
  out["v_acc"] = static_cast<double>(gps.v_acc) / 1000.0;
  telemetry["gps"] = std::move(out);
}

void AddGlobalPosition(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.global_position_int) {
    return;
  }
  const auto& pos = *state.global_position_int;
  telemetry["global_position"] = {
      {"time_boot_ms", pos.time_boot_ms},
      {"lat", static_cast<double>(pos.lat) / 1e7},
      {"lon", static_cast<double>(pos.lon) / 1e7},
      {"alt", static_cast<double>(pos.alt) / 1000.0},
      {"hdg", static_cast<double>(pos.hdg) / 100.0},
  };
}
```

`json_serializer.cpp` 顶部加 `#include <cstdint>`（`UINT16_MAX` 需要）。在 `ToJson()` 里 `AddAttitude(telemetry, state);` 之后加：

```cpp
  AddGps(telemetry, state);
  AddGlobalPosition(telemetry, state);
```

- [ ] **Step 4: 运行测试确认通过**

```bash
cmake --build build
ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: 全部 PASS。

- [ ] **Step 5: Commit**

```bash
git add src/payload/json_serializer.cpp tests/test_json_serializer.cpp
git commit -m "$(cat <<'EOF'
feat: json_serializer补充telemetry.gps/global_position

gps.eph/epv命中UINT16_MAX哨兵值时输出null；vel/cog/yaw/vel_acc/
hdg_acc(gps)、vx/vy/vz/relative_alt(global_position)不输出，
箱子固定不动，这些字段没有意义。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: `telemetry.sys_status`

**Files:**
- Modify: `src/payload/json_serializer.cpp`
- Modify: `tests/test_json_serializer.cpp`

**Interfaces:**
- Consumes: `state::TelemetryState::sys_status`。
- Produces: `AddSysStatus`。

- [ ] **Step 1: 写测试**

```cpp
TEST_CASE("sys_status字段换算,current_battery命中哨兵值-1时输出null") {
  state::TelemetryState state{};
  mavlink_sys_status_t sys{};
  sys.onboard_control_sensors_present = 1483;
  sys.onboard_control_sensors_enabled = 1483;
  sys.onboard_control_sensors_health = 1483;
  sys.load = 235;
  sys.voltage_battery = 12600;
  sys.current_battery = -1;
  sys.drop_rate_comm = 1;
  sys.errors_comm = 0;
  sys.errors_count1 = 0;
  sys.errors_count2 = 0;
  sys.errors_count3 = 0;
  sys.errors_count4 = 0;
  sys.battery_remaining = 78;
  sys.onboard_control_sensors_present_extended = 0;
  sys.onboard_control_sensors_enabled_extended = 0;
  sys.onboard_control_sensors_health_extended = 0;
  state.sys_status = sys;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["telemetry"]["sys_status"];

  CHECK(out["onboard_control_sensors_present"] == 1483);
  CHECK(out["load"].get<double>() == doctest::Approx(23.5));
  CHECK(out["voltage_battery"].get<double>() == doctest::Approx(12.6));
  CHECK(out["current_battery"].is_null());
  CHECK(out["drop_rate_comm"].get<double>() == doctest::Approx(0.1));
  CHECK(out["battery_remaining"] == 78);
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: FAIL。

- [ ] **Step 3: 实现**

```cpp
void AddSysStatus(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.sys_status) {
    return;
  }
  const auto& sys = *state.sys_status;
  nlohmann::json out;
  out["onboard_control_sensors_present"] = sys.onboard_control_sensors_present;
  out["onboard_control_sensors_enabled"] = sys.onboard_control_sensors_enabled;
  out["onboard_control_sensors_health"] = sys.onboard_control_sensors_health;
  out["load"] = static_cast<double>(sys.load) / 10.0;
  out["voltage_battery"] = static_cast<double>(sys.voltage_battery) / 1000.0;
  out["current_battery"] = (sys.current_battery == -1)
                                ? nlohmann::json(nullptr)
                                : nlohmann::json(static_cast<double>(sys.current_battery) / 100.0);
  out["drop_rate_comm"] = static_cast<double>(sys.drop_rate_comm) / 10.0;
  out["errors_comm"] = sys.errors_comm;
  out["errors_count1"] = sys.errors_count1;
  out["errors_count2"] = sys.errors_count2;
  out["errors_count3"] = sys.errors_count3;
  out["errors_count4"] = sys.errors_count4;
  out["battery_remaining"] = sys.battery_remaining;
  out["onboard_control_sensors_present_extended"] = sys.onboard_control_sensors_present_extended;
  out["onboard_control_sensors_enabled_extended"] = sys.onboard_control_sensors_enabled_extended;
  out["onboard_control_sensors_health_extended"] = sys.onboard_control_sensors_health_extended;
  telemetry["sys_status"] = std::move(out);
}
```

`ToJson()` 里加一行 `AddSysStatus(telemetry, state);`。

- [ ] **Step 4: 运行测试确认通过**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: 全部 PASS。

- [ ] **Step 5: Commit**

```bash
git add src/payload/json_serializer.cpp tests/test_json_serializer.cpp
git commit -m "$(cat <<'EOF'
feat: json_serializer补充telemetry.sys_status

current_battery命中-1哨兵值时输出null；三组传感器位图保持原始
数字；load/drop_rate_comm按x10换算成百分比。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: `telemetry.battery` + `telemetry.battery2`（共用同一个 helper）

这是电池2改走官方通道后的核心改动：`battery`/`battery2` 都来自 `state.battery_status[0]`/`[1]`，字段、换算、哨兵规则完全一样，写一个共用的 `BuildBatteryStatusJson` helper。

**Files:**
- Modify: `src/payload/json_serializer.cpp`
- Modify: `tests/test_json_serializer.cpp`

**Interfaces:**
- Consumes: `state::TelemetryState::battery_status`(`std::array<std::optional<mavlink_battery_status_t>, state::kBatteryCount>`，Task 1 已经改好)。
- Produces: `AddBattery`（写 `telemetry.battery`，来自 `battery_status[0]`）、`AddBattery2`（写 `telemetry.battery2`，来自 `battery_status[1]`）；内部共用 `BuildBatteryStatusJson(const mavlink_battery_status_t&) -> nlohmann::json`。

- [ ] **Step 1: 写测试**

```cpp
TEST_CASE("battery字段换算含哨兵值,只收到battery_status[0]时battery2不存在") {
  state::TelemetryState state{};
  mavlink_battery_status_t bs{};
  bs.current_consumed = 1520;
  bs.energy_consumed = 185;  // hJ -> J: *100 = 18500
  bs.temperature = 2850;
  std::uint16_t voltages[10] = {4150, 4140, 4150, 4130, 65535, 65535, 65535, 65535, 65535, 65535};
  std::memcpy(bs.voltages, voltages, sizeof(voltages));
  bs.current_battery = 325;
  bs.id = 0;
  bs.battery_function = 1;
  bs.type = 1;
  bs.battery_remaining = 78;
  bs.time_remaining = 3600;
  bs.charge_state = 2;
  std::uint16_t voltages_ext[4] = {0, 0, 0, 0};
  std::memcpy(bs.voltages_ext, voltages_ext, sizeof(voltages_ext));
  bs.mode = 0;
  bs.fault_bitmask = 0;
  state.battery_status[0] = bs;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["telemetry"]["battery"];

  CHECK(out["current_consumed"] == 1520);
  CHECK(out["energy_consumed"].get<double>() == doctest::Approx(18500.0));
  CHECK(out["temperature"].get<double>() == doctest::Approx(28.5));
  CHECK(out["voltages"][0].get<double>() == doctest::Approx(4.15));
  CHECK(out["voltages"][4].is_null());
  CHECK(out["current_battery"].get<double>() == doctest::Approx(3.25));
  CHECK(out["id"] == 0);
  CHECK(out["battery_remaining"] == 78);
  CHECK(out["voltages_ext"][0].is_null());
  CHECK_FALSE(json["telemetry"].contains("battery2"));
}

TEST_CASE("battery2跟battery用同一套规则,来自battery_status[1]") {
  state::TelemetryState state{};
  mavlink_battery_status_t bs{};
  bs.current_battery = -1;  // 哨兵值 -> null
  bs.battery_remaining = -1;  // 哨兵值 -> null
  bs.id = 1;
  std::uint16_t voltages[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  std::memcpy(bs.voltages, voltages, sizeof(voltages));
  std::uint16_t voltages_ext[4] = {0, 0, 0, 0};
  std::memcpy(bs.voltages_ext, voltages_ext, sizeof(voltages_ext));
  state.battery_status[1] = bs;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["telemetry"]["battery2"];

  CHECK(out["id"] == 1);
  CHECK(out["current_battery"].is_null());
  CHECK(out["battery_remaining"].is_null());
  CHECK_FALSE(json["telemetry"].contains("battery"));
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: FAIL。

- [ ] **Step 3: 实现**

`json_serializer.cpp` 顶部加 `#include <cstring>`（测试用到 `memcpy`，实现本身不需要，但如果放在同一批 include 里可以不加也行——这里只是提醒实现文件不需要这个头）。在 `AddSysStatus` 后面新增：

```cpp
nlohmann::json BuildBatteryStatusJson(const mavlink_battery_status_t& bs) {
  nlohmann::json out;
  out["current_consumed"] = bs.current_consumed;
  out["energy_consumed"] = static_cast<double>(bs.energy_consumed) * 100.0;
  out["temperature"] = static_cast<double>(bs.temperature) / 100.0;

  nlohmann::json voltages = nlohmann::json::array();
  for (std::uint16_t v : bs.voltages) {
    voltages.push_back((v == UINT16_MAX) ? nlohmann::json(nullptr)
                                          : nlohmann::json(static_cast<double>(v) / 1000.0));
  }
  out["voltages"] = std::move(voltages);

  out["current_battery"] = (bs.current_battery == -1)
                                ? nlohmann::json(nullptr)
                                : nlohmann::json(static_cast<double>(bs.current_battery) / 100.0);
  out["id"] = bs.id;
  out["battery_function"] = bs.battery_function;
  out["type"] = bs.type;
  out["battery_remaining"] =
      (bs.battery_remaining == -1) ? nlohmann::json(nullptr) : nlohmann::json(bs.battery_remaining);
  out["time_remaining"] = bs.time_remaining;
  out["charge_state"] = bs.charge_state;

  nlohmann::json voltages_ext = nlohmann::json::array();
  for (std::uint16_t v : bs.voltages_ext) {
    voltages_ext.push_back((v == 0) ? nlohmann::json(nullptr)
                                     : nlohmann::json(static_cast<double>(v) / 1000.0));
  }
  out["voltages_ext"] = std::move(voltages_ext);

  out["mode"] = bs.mode;
  out["fault_bitmask"] = bs.fault_bitmask;
  return out;
}

void AddBattery(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (state.battery_status[0]) {
    telemetry["battery"] = BuildBatteryStatusJson(*state.battery_status[0]);
  }
}

void AddBattery2(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (state.battery_status[1]) {
    telemetry["battery2"] = BuildBatteryStatusJson(*state.battery_status[1]);
  }
}
```

`ToJson()` 里加两行：

```cpp
  AddBattery(telemetry, state);
  AddBattery2(telemetry, state);
```

- [ ] **Step 4: 运行测试确认通过**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: 全部 PASS。

- [ ] **Step 5: Commit**

```bash
git add src/payload/json_serializer.cpp tests/test_json_serializer.cpp
git commit -m "$(cat <<'EOF'
feat: json_serializer补充telemetry.battery/battery2

battery2改用跟battery完全相同的BATTERY_STATUS字段/换算规则，
来自battery_status[1]，不再是简化的自定义结构，对齐固件即将
切换到的id=0/id=1双BATTERY_STATUS方案。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: `telemetry.pressure`

**Files:**
- Modify: `src/payload/json_serializer.cpp`
- Modify: `tests/test_json_serializer.cpp`

**Interfaces:**
- Consumes: `state::TelemetryState::scaled_pressure`。
- Produces: `AddPressure`。

- [ ] **Step 1: 写测试**

```cpp
TEST_CASE("pressure字段换算,press_abs/press_diff已是hPa直接透传") {
  state::TelemetryState state{};
  mavlink_scaled_pressure_t p{};
  p.time_boot_ms = 123456;
  p.press_abs = 1013.25F;
  p.press_diff = 0.02F;
  p.temperature = 2650;
  p.temperature_press_diff = 2650;
  state.scaled_pressure = p;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["telemetry"]["pressure"];

  CHECK(out["time_boot_ms"] == 123456);
  CHECK(out["press_abs"].get<double>() == doctest::Approx(1013.25));
  CHECK(out["press_diff"].get<double>() == doctest::Approx(0.02));
  CHECK(out["temperature"].get<double>() == doctest::Approx(26.5));
  CHECK(out["temperature_press_diff"].get<double>() == doctest::Approx(26.5));
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: FAIL。

- [ ] **Step 3: 实现**

```cpp
void AddPressure(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.scaled_pressure) {
    return;
  }
  const auto& p = *state.scaled_pressure;
  telemetry["pressure"] = {
      {"time_boot_ms", p.time_boot_ms},
      {"press_abs", static_cast<double>(p.press_abs)},
      {"press_diff", static_cast<double>(p.press_diff)},
      {"temperature", static_cast<double>(p.temperature) / 100.0},
      {"temperature_press_diff", static_cast<double>(p.temperature_press_diff) / 100.0},
  };
}
```

`ToJson()` 里加 `AddPressure(telemetry, state);`。

- [ ] **Step 4: 运行测试确认通过**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: 全部 PASS。

- [ ] **Step 5: Commit**

```bash
git add src/payload/json_serializer.cpp tests/test_json_serializer.cpp
git commit -m "feat: json_serializer补充telemetry.pressure

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

---

## Task 9: `telemetry.gnss_sat/humidity/motor/lora/remote_id`（自定义扩展字段）+ 模块状态字符串 helper

`lora.link_state` 要复用模块状态字符串表，这个 Task 顺便把 `ModuleStateToString` 这个共用 helper 写出来（Task 10 的 `modules` 数组也要用）。

**Files:**
- Modify: `src/payload/json_serializer.cpp`
- Modify: `tests/test_json_serializer.cpp`

**Interfaces:**
- Consumes: `state::TelemetryState::gnss_sat/env_humidity/motor_pwm/lora_status/remote_id_status`。
- Produces: `ModuleStateToString(std::uint8_t) -> std::string`（Task 10 会复用）；`AddGnssSat`、`AddHumidity`、`AddMotor`、`AddLora`、`AddRemoteId`。

- [ ] **Step 1: 写测试**

```cpp
TEST_CASE("gnss_sat/humidity/motor/lora/remote_id自定义字段") {
  state::TelemetryState state{};
  state.gnss_sat = state::GnssSat{9, 8, 7, 6};
  state.env_humidity = state::EnvHumidity{535};
  state.motor_pwm = state::MotorPwm{{45, 45, 50, 50}, true, 60};
  state.lora_status = state::LoraStatus{15, 9, true, 2};  // link_state=2 -> "ONLINE"
  state.remote_id_status = state::RemoteIdStatus{120, 0, 987654};

  auto json = payload::ToJson(state, "NNUTC");
  const auto& t = json["telemetry"];

  CHECK(t["gnss_sat"]["gps_visible"] == 9);
  CHECK(t["gnss_sat"]["beidou_used"] == 6);
  CHECK(t["humidity"]["humidity_percent"].get<double>() == doctest::Approx(53.5));
  CHECK(t["motor"]["duty_percent"] == std::vector<int>{45, 45, 50, 50});
  CHECK(t["motor"]["run_state"] == true);
  CHECK(t["motor"]["speed_level"] == 60);
  CHECK(t["lora"]["loss_rate_percent"].get<double>() == doctest::Approx(1.5));
  CHECK(t["lora"]["node_id"] == 9);
  CHECK(t["lora"]["present"] == true);
  CHECK(t["lora"]["link_state"] == "ONLINE");
  CHECK(t["remote_id"]["location_count"] == 120);
  CHECK(t["remote_id"]["last_success_ms"] == 987654);
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: FAIL。

- [ ] **Step 3: 实现**

```cpp
constexpr std::array<std::string_view, 7> kModuleStateNames = {
    "UNINITIALIZED", "STARTING", "ONLINE", "DEGRADED", "OFFLINE", "FAILED", "DISABLED"};

std::string ModuleStateToString(std::uint8_t state) {
  if (state < kModuleStateNames.size()) {
    return std::string(kModuleStateNames[state]);
  }
  return "UNKNOWN(" + std::to_string(state) + ")";
}

void AddGnssSat(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.gnss_sat) {
    return;
  }
  const auto& s = *state.gnss_sat;
  telemetry["gnss_sat"] = {
      {"gps_visible", s.gps_visible},
      {"beidou_visible", s.beidou_visible},
      {"gps_used", s.gps_used},
      {"beidou_used", s.beidou_used},
  };
}

void AddHumidity(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.env_humidity) {
    return;
  }
  telemetry["humidity"]["humidity_percent"] =
      static_cast<double>(state.env_humidity->relative_humidity_x10) / 10.0;
}

void AddMotor(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.motor_pwm) {
    return;
  }
  const auto& m = *state.motor_pwm;
  telemetry["motor"] = {
      {"duty_percent", m.duty_percent},
      {"run_state", m.run_state},
      {"speed_level", m.speed_level},
  };
}

void AddLora(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.lora_status) {
    return;
  }
  const auto& l = *state.lora_status;
  telemetry["lora"] = {
      {"loss_rate_percent", static_cast<double>(l.loss_rate_x10) / 10.0},
      {"node_id", l.node_id},
      {"present", l.present},
      {"link_state", ModuleStateToString(l.link_state)},
  };
}

void AddRemoteId(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.remote_id_status) {
    return;
  }
  const auto& r = *state.remote_id_status;
  telemetry["remote_id"] = {
      {"location_count", r.location_count},
      {"error_count", r.error_count},
      {"last_success_ms", r.last_success_ms},
  };
}
```

`json_serializer.cpp` 顶部加 `#include <array>`、`#include <string>`、`#include <string_view>`。`ToJson()` 里加：

```cpp
  AddGnssSat(telemetry, state);
  AddHumidity(telemetry, state);
  AddMotor(telemetry, state);
  AddLora(telemetry, state);
  AddRemoteId(telemetry, state);
```

- [ ] **Step 4: 运行测试确认通过**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: 全部 PASS。

- [ ] **Step 5: Commit**

```bash
git add src/payload/json_serializer.cpp tests/test_json_serializer.cpp
git commit -m "$(cat <<'EOF'
feat: json_serializer补充自定义扩展字段+模块状态字符串helper

gnss_sat/humidity/motor/lora/remote_id都是自定义struct，字段名
自己定；顺便写了ModuleStateToString，lora.link_state复用这张表，
下一个Task的modules数组也会用它。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: 顶层 `modules` 数组

**Files:**
- Modify: `src/payload/json_serializer.cpp`
- Modify: `tests/test_json_serializer.cpp`

**Interfaces:**
- Consumes: `state::TelemetryState::module_status`(`std::optional<std::array<std::uint8_t, state::kModuleCount>>`)。
- Produces: `BuildModules(const state::TelemetryState&) -> nlohmann::json`（复用 Task 9 的 `ModuleStateToString`）。

- [ ] **Step 1: 写测试**

```cpp
TEST_CASE("modules数组:14个模块的name/status,未收到module_status时modules不存在") {
  state::TelemetryState empty{};
  auto empty_json = payload::ToJson(empty, "NNUTC");
  CHECK_FALSE(empty_json.contains("modules"));

  state::TelemetryState state{};
  std::array<std::uint8_t, 14> mods{};
  mods.fill(0);
  mods[0] = 2;  // GNSS=ONLINE
  mods[4] = 3;  // LORA=DEGRADED
  state.module_status = mods;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("modules"));
  REQUIRE(json["modules"].size() == 14);
  CHECK(json["modules"][0]["name"] == "GNSS");
  CHECK(json["modules"][0]["status"] == "ONLINE");
  CHECK(json["modules"][4]["name"] == "LORA");
  CHECK(json["modules"][4]["status"] == "DEGRADED");
  CHECK(json["modules"][1]["name"] == "IMU");
  CHECK(json["modules"][1]["status"] == "UNINITIALIZED");  // 零初始化占位，合法语义
  CHECK(json["modules"][13]["name"] == "BUSINESS");
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: FAIL。

- [ ] **Step 3: 实现**

```cpp
constexpr std::array<std::string_view, 14> kModuleNames = {
    "GNSS",     "IMU",       "BARO",  "BATTERY",   "LORA",      "5G",        "STORAGE",
    "REMOTE_ID", "DISPLAY",  "CONTROL", "ALARM",   "SYSTEM",    "ESTIMATOR", "BUSINESS"};

nlohmann::json BuildModules(const state::TelemetryState& state) {
  nlohmann::json modules = nlohmann::json::array();
  for (std::size_t i = 0; i < state.module_status->size(); ++i) {
    modules.push_back({
        {"name", kModuleNames[i]},
        {"status", ModuleStateToString((*state.module_status)[i])},
    });
  }
  return modules;
}
```

`ToJson()` 里在 `telemetry` 相关代码块之后新增：

```cpp
  if (state.module_status) {
    out["modules"] = BuildModules(state);
  }
```

- [ ] **Step 4: 运行测试确认通过**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: 全部 PASS。

- [ ] **Step 5: Commit**

```bash
git add src/payload/json_serializer.cpp tests/test_json_serializer.cpp
git commit -m "feat: json_serializer补充顶层modules数组

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

---

## Task 11: 顶层 `alarms` + `logs`

**Files:**
- Modify: `src/payload/json_serializer.cpp`
- Modify: `tests/test_json_serializer.cpp`

**Interfaces:**
- Consumes: `state::TelemetryState::alarm_table`(`std::optional<state::AlarmTable>`)、`::message_log`(`std::optional<state::MessageLog>`)。
- Produces: `BuildAlarms`、`BuildLogs`。

- [ ] **Step 1: 写测试**

```cpp
TEST_CASE("alarms按active_count截断,未收到时不存在") {
  state::TelemetryState empty{};
  CHECK_FALSE(payload::ToJson(empty, "NNUTC").contains("alarms"));

  state::TelemetryState state{};
  state::AlarmTable table{};
  table.ver = 1;
  table.active_count = 2;
  table.entries[0] = state::AlarmEntry{4, 1032, 2, true, 15};
  table.entries[1] = state::AlarmEntry{9, 2004, 1, false, 320};
  state.alarm_table = table;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("alarms"));
  CHECK(json["alarms"]["ver"] == 1);
  REQUIRE(json["alarms"]["entries"].size() == 2);
  CHECK(json["alarms"]["entries"][0]["source_id"] == 4);
  CHECK(json["alarms"]["entries"][0]["fault_code"] == 1032);
  CHECK(json["alarms"]["entries"][0]["active"] == true);
  CHECK(json["alarms"]["entries"][1]["age_s"] == 320);
}

TEST_CASE("logs按count截断,time格式化成HH:MM:SS,未收到时不存在") {
  state::TelemetryState empty{};
  CHECK_FALSE(payload::ToJson(empty, "NNUTC").contains("logs"));

  state::TelemetryState state{};
  state::MessageLog log{};
  log.latest_seq = 458;
  log.count = 1;
  log.entries[0] = state::LogEntry{456, 12, {14, 23, 7}, 1};
  state.message_log = log;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("logs"));
  CHECK(json["logs"]["latest_seq"] == 458);
  REQUIRE(json["logs"]["entries"].size() == 1);
  CHECK(json["logs"]["entries"][0]["sequence"] == 456);
  CHECK(json["logs"]["entries"][0]["message_id"] == 12);
  CHECK(json["logs"]["entries"][0]["time"] == "14:23:07");
  CHECK(json["logs"]["entries"][0]["severity"] == 1);
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: FAIL。

- [ ] **Step 3: 实现**

`json_serializer.cpp` 顶部加 `#include <cstdio>`（`snprintf` 格式化时间）。新增：

```cpp
nlohmann::json BuildAlarms(const state::AlarmTable& table) {
  nlohmann::json entries = nlohmann::json::array();
  for (std::size_t i = 0; i < table.active_count; ++i) {
    const auto& e = table.entries[i];
    entries.push_back({
        {"source_id", e.source_id},
        {"fault_code", e.fault_code},
        {"severity", e.severity},
        {"active", e.active},
        {"age_s", e.age_s},
    });
  }
  return {{"ver", table.ver}, {"entries", std::move(entries)}};
}

std::string FormatTimeHhMmSs(const std::array<std::uint8_t, 3>& hms) {
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hms[0], hms[1], hms[2]);
  return std::string(buf);
}

nlohmann::json BuildLogs(const state::MessageLog& log) {
  nlohmann::json entries = nlohmann::json::array();
  for (std::size_t i = 0; i < log.count; ++i) {
    const auto& e = log.entries[i];
    entries.push_back({
        {"sequence", e.sequence},
        {"message_id", e.message_id},
        {"time", FormatTimeHhMmSs(e.time_hhmmss)},
        {"severity", e.severity},
    });
  }
  return {{"latest_seq", log.latest_seq}, {"entries", std::move(entries)}};
}
```

`ToJson()` 里加：

```cpp
  if (state.alarm_table) {
    out["alarms"] = BuildAlarms(*state.alarm_table);
  }
  if (state.message_log) {
    out["logs"] = BuildLogs(*state.message_log);
  }
```

- [ ] **Step 4: 运行测试确认通过**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: 全部 PASS。

- [ ] **Step 5: Commit**

```bash
git add src/payload/json_serializer.cpp tests/test_json_serializer.cpp
git commit -m "feat: json_serializer补充顶层alarms/logs

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

---

## Task 12: `drone_id`（`basic_id`/`location`/`system`/`operator_id`/`self_id`）

**Files:**
- Modify: `src/payload/json_serializer.cpp`
- Modify: `tests/test_json_serializer.cpp`

**Interfaces:**
- Consumes: `state::TelemetryState::open_drone_id_basic_id/location/system/operator_id/self_id`。
- Produces: `ToHexString(const std::uint8_t*, std::size_t) -> std::string`、`ToTrimmedString(const char*, std::size_t) -> std::string`、`AddDroneIdBasicId`/`AddDroneIdLocation`/`AddDroneIdSystem`/`AddDroneIdOperatorId`/`AddDroneIdSelfId`（这 5 个函数签名是 `(nlohmann::json& drone_id, const state::TelemetryState& state)`，跟 `telemetry` 那批不同，操作的是 `drone_id` 这个局部对象）。

- [ ] **Step 1: 写测试**

```cpp
TEST_CASE("drone_id.basic_id:id_or_mac转十六进制,uas_id去除尾部空字符") {
  state::TelemetryState state{};
  mavlink_open_drone_id_basic_id_t basic{};
  basic.target_system = 0;
  basic.target_component = 0;
  std::memset(basic.id_or_mac, 0, sizeof(basic.id_or_mac));
  basic.id_type = 1;
  basic.ua_type = 2;
  std::memcpy(basic.uas_id, "DCDWCNS1AB12CD34EF56", 20);
  state.open_drone_id_basic_id = basic;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["drone_id"]["basic_id"];

  CHECK(out["id_or_mac"] == "0000000000000000000000000000000000000000");
  CHECK(out["id_or_mac"].get<std::string>().size() == 40);
  CHECK(out["id_type"] == 1);
  CHECK(out["ua_type"] == 2);
  CHECK(out["uas_id"] == "DCDWCNS1AB12CD34EF56");
}

TEST_CASE("drone_id.location:altitude哨兵值-1000转null,speed/direction/height等字段不输出") {
  state::TelemetryState state{};
  mavlink_open_drone_id_location_t loc{};
  loc.latitude = 399042000;
  loc.longitude = 1164074000;
  loc.altitude_barometric = -1000.0F;  // 哨兵值 -> null
  loc.altitude_geodetic = 44.8F;
  loc.timestamp = 1234.5F;
  loc.status = 2;
  loc.horizontal_accuracy = 4;
  loc.vertical_accuracy = 4;
  loc.barometer_accuracy = 3;
  loc.timestamp_accuracy = 2;
  state.open_drone_id_location = loc;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["drone_id"]["location"];

  CHECK(out["latitude"].get<double>() == doctest::Approx(39.9042));
  CHECK(out["altitude_barometric"].is_null());
  CHECK(out["altitude_geodetic"].get<double>() == doctest::Approx(44.8));
  CHECK(out["timestamp"].get<double>() == doctest::Approx(1234.5));
  CHECK(out["status"] == 2);
  CHECK_FALSE(out.contains("speed_horizontal"));
  CHECK_FALSE(out.contains("speed_vertical"));
  CHECK_FALSE(out.contains("direction"));
  CHECK_FALSE(out.contains("height"));
  CHECK_FALSE(out.contains("height_reference"));
  CHECK_FALSE(out.contains("speed_accuracy"));
}

TEST_CASE("drone_id.system:operator_altitude_geo哨兵值,area_*字段不输出") {
  state::TelemetryState state{};
  mavlink_open_drone_id_system_t sys{};
  sys.operator_latitude = 399050000;
  sys.operator_longitude = 1164080000;
  sys.operator_altitude_geo = -1000.0F;  // 哨兵值 -> null
  sys.timestamp = 233366400;
  sys.operator_location_type = 0;
  sys.classification_type = 0;
  sys.category_eu = 0;
  sys.class_eu = 0;
  state.open_drone_id_system = sys;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["drone_id"]["system"];

  CHECK(out["operator_latitude"].get<double>() == doctest::Approx(39.905));
  CHECK(out["operator_altitude_geo"].is_null());
  CHECK(out["timestamp"] == 233366400);
  CHECK_FALSE(out.contains("area_ceiling"));
  CHECK_FALSE(out.contains("area_floor"));
  CHECK_FALSE(out.contains("area_count"));
  CHECK_FALSE(out.contains("area_radius"));
}

TEST_CASE("drone_id.operator_id/self_id:文本字段去除尾部空字符") {
  state::TelemetryState state{};
  mavlink_open_drone_id_operator_id_t op{};
  op.operator_id_type = 0;
  std::memset(op.operator_id, 0, sizeof(op.operator_id));
  std::memcpy(op.operator_id, "CAAB1234567890", 14);
  state.open_drone_id_operator_id = op;

  mavlink_open_drone_id_self_id_t self{};
  self.description_type = 0;
  std::memset(self.description, 0, sizeof(self.description));
  std::memcpy(self.description, "CNS-RPI training kit", 21);
  state.open_drone_id_self_id = self;

  auto json = payload::ToJson(state, "NNUTC");

  CHECK(json["drone_id"]["operator_id"]["operator_id"] == "CAAB1234567890");
  CHECK(json["drone_id"]["self_id"]["description"] == "CNS-RPI training kit");
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: FAIL。

- [ ] **Step 3: 实现**

`json_serializer.cpp` 顶部加 `#include <cstring>`（`strnlen`）。新增：

```cpp
std::string ToHexString(const std::uint8_t* data, std::size_t len) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string out(len * 2, '0');
  for (std::size_t i = 0; i < len; ++i) {
    out[i * 2] = kHexDigits[(data[i] >> 4) & 0xF];
    out[i * 2 + 1] = kHexDigits[data[i] & 0xF];
  }
  return out;
}

std::string ToTrimmedString(const char* data, std::size_t max_len) {
  return std::string(data, strnlen(data, max_len));
}

void AddDroneIdBasicId(nlohmann::json& drone_id, const state::TelemetryState& state) {
  if (!state.open_drone_id_basic_id) {
    return;
  }
  const auto& b = *state.open_drone_id_basic_id;
  drone_id["basic_id"] = {
      {"target_system", b.target_system},
      {"target_component", b.target_component},
      {"id_or_mac", ToHexString(b.id_or_mac, sizeof(b.id_or_mac))},
      {"id_type", b.id_type},
      {"ua_type", b.ua_type},
      {"uas_id", ToTrimmedString(reinterpret_cast<const char*>(b.uas_id), sizeof(b.uas_id))},
  };
}

void AddDroneIdLocation(nlohmann::json& drone_id, const state::TelemetryState& state) {
  if (!state.open_drone_id_location) {
    return;
  }
  const auto& l = *state.open_drone_id_location;
  nlohmann::json out;
  out["latitude"] = static_cast<double>(l.latitude) / 1e7;
  out["longitude"] = static_cast<double>(l.longitude) / 1e7;
  out["altitude_barometric"] = (l.altitude_barometric == -1000.0F)
                                    ? nlohmann::json(nullptr)
                                    : nlohmann::json(static_cast<double>(l.altitude_barometric));
  out["altitude_geodetic"] = (l.altitude_geodetic == -1000.0F)
                                  ? nlohmann::json(nullptr)
                                  : nlohmann::json(static_cast<double>(l.altitude_geodetic));
  out["timestamp"] = static_cast<double>(l.timestamp);
  out["target_system"] = l.target_system;
  out["target_component"] = l.target_component;
  out["id_or_mac"] = ToHexString(l.id_or_mac, sizeof(l.id_or_mac));
  out["status"] = l.status;
  out["horizontal_accuracy"] = l.horizontal_accuracy;
  out["vertical_accuracy"] = l.vertical_accuracy;
  out["barometer_accuracy"] = l.barometer_accuracy;
  out["timestamp_accuracy"] = l.timestamp_accuracy;
  drone_id["location"] = std::move(out);
}

void AddDroneIdSystem(nlohmann::json& drone_id, const state::TelemetryState& state) {
  if (!state.open_drone_id_system) {
    return;
  }
  const auto& s = *state.open_drone_id_system;
  nlohmann::json out;
  out["operator_latitude"] = static_cast<double>(s.operator_latitude) / 1e7;
  out["operator_longitude"] = static_cast<double>(s.operator_longitude) / 1e7;
  out["operator_altitude_geo"] = (s.operator_altitude_geo == -1000.0F)
                                      ? nlohmann::json(nullptr)
                                      : nlohmann::json(static_cast<double>(s.operator_altitude_geo));
  out["timestamp"] = s.timestamp;
  out["target_system"] = s.target_system;
  out["target_component"] = s.target_component;
  out["id_or_mac"] = ToHexString(s.id_or_mac, sizeof(s.id_or_mac));
  out["operator_location_type"] = s.operator_location_type;
  out["classification_type"] = s.classification_type;
  out["category_eu"] = s.category_eu;
  out["class_eu"] = s.class_eu;
  drone_id["system"] = std::move(out);
}

void AddDroneIdOperatorId(nlohmann::json& drone_id, const state::TelemetryState& state) {
  if (!state.open_drone_id_operator_id) {
    return;
  }
  const auto& o = *state.open_drone_id_operator_id;
  drone_id["operator_id"] = {
      {"target_system", o.target_system},
      {"target_component", o.target_component},
      {"id_or_mac", ToHexString(o.id_or_mac, sizeof(o.id_or_mac))},
      {"operator_id_type", o.operator_id_type},
      {"operator_id", ToTrimmedString(o.operator_id, sizeof(o.operator_id))},
  };
}

void AddDroneIdSelfId(nlohmann::json& drone_id, const state::TelemetryState& state) {
  if (!state.open_drone_id_self_id) {
    return;
  }
  const auto& s = *state.open_drone_id_self_id;
  drone_id["self_id"] = {
      {"target_system", s.target_system},
      {"target_component", s.target_component},
      {"id_or_mac", ToHexString(s.id_or_mac, sizeof(s.id_or_mac))},
      {"description_type", s.description_type},
      {"description", ToTrimmedString(s.description, sizeof(s.description))},
  };
}
```

`ToJson()` 里，在 `modules`/`alarms`/`logs` 那几行之后新增：

```cpp
  nlohmann::json drone_id = nlohmann::json::object();
  AddDroneIdBasicId(drone_id, state);
  AddDroneIdLocation(drone_id, state);
  AddDroneIdSystem(drone_id, state);
  AddDroneIdOperatorId(drone_id, state);
  AddDroneIdSelfId(drone_id, state);
  if (!drone_id.empty()) {
    out["drone_id"] = std::move(drone_id);
  }
```

- [ ] **Step 4: 运行测试确认通过**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: 全部 PASS。

- [ ] **Step 5: Commit**

```bash
git add src/payload/json_serializer.cpp tests/test_json_serializer.cpp
git commit -m "$(cat <<'EOF'
feat: json_serializer补充drone_id(basic_id/location/system/operator_id/self_id)

id_or_mac转十六进制字符串；uas_id/operator_id/description去除尾部
空字符；altitude_barometric/altitude_geodetic/operator_altitude_geo
命中-1000.0f哨兵值时输出null；speed/direction/height/area_*等
运动学字段不输出(箱子固定不动，见设计文档第1节)。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 13: `main.cpp` 集成——每帧打印 JSON

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `payload::ToJson(const state::TelemetryState&, const std::string&)`（Task 3-12 已经实现完整）、`config::AppConfig::identity.school_name`（Task 2 已经实现）。

- [ ] **Step 1: 改 `main.cpp`**

在 `#include` 部分加一行：

```cpp
#include "payload/json_serializer.hpp"
```

在 `LogExtension` 函数定义之后（匿名命名空间结束之前）新增：

```cpp
/// 每处理完一帧调用一次，把当前完整快照序列化成JSON打印到控制台，
/// 供真机演示/联调时人工核对字段可读性——不是解码逻辑，纯打印。
void LogJsonPayload(const state::TelemetryState& state, const std::string& school_name) {
  std::cout << payload::ToJson(state, school_name).dump(2) << std::endl;
}
```

在 `main()` 函数的主循环里，把：

```cpp
    if (auto msg = link->ReceiveMessage()) {
      state_store.UpdateDcdwLabel(protocol::FormatDcdwLabel(msg->sysid));
      if (protocol::DecodeAndStore(*msg, state_store)) {
        LogTelemetry(msg->msgid, state_store.Snapshot());
      } else if (protocol::DecodeExtensionAndStore(*msg, state_store)) {
        LogExtension(msg->msgid, state_store.Snapshot());
      }
    }
```

改成：

```cpp
    if (auto msg = link->ReceiveMessage()) {
      state_store.UpdateDcdwLabel(protocol::FormatDcdwLabel(msg->sysid));
      if (protocol::DecodeAndStore(*msg, state_store)) {
        LogTelemetry(msg->msgid, state_store.Snapshot());
      } else if (protocol::DecodeExtensionAndStore(*msg, state_store)) {
        LogExtension(msg->msgid, state_store.Snapshot());
      }
      LogJsonPayload(state_store.Snapshot(), app_config->identity.school_name);
    }
```

（`LogJsonPayload` 在 `if/else if` 外面、无条件调用——不管这一帧是不是本项目认识的消息类型，只要收到一帧就重新序列化打印一次，对应设计文档"没有凑齐才输出的门槛"这条规则；`dcdw_label` 已经在上面更新过，所以这一帧的 `identity.dcdw_label` 会是最新的。）

- [ ] **Step 2: 编译**

```bash
cmake --build build
```

Expected: 编译成功，没有警告（`-Wall -Wextra` 全过）。

- [ ] **Step 3: 真机/模拟器人工验证（不是自动化测试，是人工核对，M4 验收标准就是"人工核对字段可读性"）**

用已有的 `tools/mavlink_sim/send_frames.py` 模拟器发几帧，跑 `./build/cns_rpi config/config.example.json`（注意 `config.example.json` 现在需要有 `identity.school_name`，Task 2 已经加过了），确认控制台每收到一帧都打印一段格式化的 JSON，字段名/数值跟设计文档第 11 节的完整示例对得上。

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
feat: main.cpp接入payload::ToJson,每帧打印JSON到控制台

M4收尾：每处理完一帧(不管是否本项目认识的消息类型)都重新
序列化一次完整快照并打印，供真机演示/联调时人工核对字段。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 14: 全字段覆盖的集成测试 + 哨兵值补充覆盖

前面每个 Task 的测试都是"这一组字段单独填充"，这个 Task 补一条全字段同时填充的集成测试（对应设计文档第 11 节的完整示例），并补上前面 Task 里没单独覆盖到的哨兵值场景（`voltages_ext`里等于 0 的槽位）。

**Files:**
- Modify: `tests/test_json_serializer.cpp`

**Interfaces:**
- 无新增函数，只加测试用例。

- [ ] **Step 1: 写测试**

```cpp
TEST_CASE("全部字段同时填充,顶层结构完整,alarms/logs按截断数正确") {
  state::TelemetryState state{};

  mavlink_heartbeat_t hb{};
  hb.type = 2;
  hb.autopilot = 12;
  hb.base_mode = 81;
  hb.system_status = 4;
  hb.mavlink_version = 3;
  state.heartbeat = hb;

  mavlink_attitude_t att{};
  att.roll = 0.1F;
  state.attitude = att;

  mavlink_gps_raw_int_t gps{};
  gps.lat = 399042000;
  gps.eph = 120;
  gps.epv = 150;
  state.gps_raw_int = gps;

  mavlink_global_position_int_t pos{};
  pos.hdg = 8750;
  state.global_position_int = pos;

  mavlink_sys_status_t sys{};
  sys.current_battery = -1;
  state.sys_status = sys;

  mavlink_battery_status_t bs0{};
  bs0.id = 0;
  bs0.current_battery = 325;
  std::uint16_t voltages_ext_nonzero[4] = {3700, 0, 0, 0};
  std::memcpy(bs0.voltages_ext, voltages_ext_nonzero, sizeof(voltages_ext_nonzero));
  state.battery_status[0] = bs0;

  mavlink_battery_status_t bs1{};
  bs1.id = 1;
  state.battery_status[1] = bs1;

  mavlink_scaled_pressure_t pressure{};
  pressure.press_abs = 1013.25F;
  state.scaled_pressure = pressure;

  state.gnss_sat = state::GnssSat{9, 8, 7, 6};
  state.env_humidity = state::EnvHumidity{535};
  state.motor_pwm = state::MotorPwm{{45, 45, 50, 50}, true, 60};
  state.lora_status = state::LoraStatus{15, 9, true, 2};
  state.remote_id_status = state::RemoteIdStatus{120, 0, 987654};

  std::array<std::uint8_t, 14> mods{};
  mods.fill(2);
  state.module_status = mods;

  state::AlarmTable alarms{};
  alarms.ver = 1;
  alarms.active_count = 1;
  alarms.entries[0] = state::AlarmEntry{4, 1032, 2, true, 15};
  state.alarm_table = alarms;

  state::MessageLog logs{};
  logs.latest_seq = 1;
  logs.count = 1;
  logs.entries[0] = state::LogEntry{1, 1, {0, 0, 1}, 0};
  state.message_log = logs;

  mavlink_open_drone_id_basic_id_t basic{};
  std::memcpy(basic.uas_id, "DCDWCNS1AB12CD34EF56", 20);
  state.open_drone_id_basic_id = basic;

  mavlink_open_drone_id_location_t loc{};
  loc.altitude_barometric = 45.2F;
  state.open_drone_id_location = loc;

  mavlink_open_drone_id_system_t odsys{};
  odsys.operator_altitude_geo = 45.0F;
  state.open_drone_id_system = odsys;

  mavlink_open_drone_id_operator_id_t op{};
  std::memcpy(op.operator_id, "CAAB1234567890", 14);
  state.open_drone_id_operator_id = op;

  mavlink_open_drone_id_self_id_t self{};
  std::memcpy(self.description, "CNS-RPI training kit", 21);
  state.open_drone_id_self_id = self;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("identity"));
  REQUIRE(json.contains("telemetry"));
  REQUIRE(json.contains("modules"));
  REQUIRE(json.contains("alarms"));
  REQUIRE(json.contains("logs"));
  REQUIRE(json.contains("drone_id"));

  const auto& t = json["telemetry"];
  CHECK(t.contains("heartbeat"));
  CHECK(t.contains("attitude"));
  CHECK(t.contains("gps"));
  CHECK(t.contains("global_position"));
  CHECK(t.contains("sys_status"));
  CHECK(t.contains("battery"));
  CHECK(t.contains("battery2"));
  CHECK(t.contains("pressure"));
  CHECK(t.contains("gnss_sat"));
  CHECK(t.contains("humidity"));
  CHECK(t.contains("motor"));
  CHECK(t.contains("lora"));
  CHECK(t.contains("remote_id"));

  // voltages_ext里非0槽位正确换算，0槽位是null(哨兵值)——前面Task 7的测试
  // 只覆盖了全0的情况，这里补上"部分槽位有值"的情况。
  CHECK(t["battery"]["voltages_ext"][0].get<double>() == doctest::Approx(3.7));
  CHECK(t["battery"]["voltages_ext"][1].is_null());

  REQUIRE(json["modules"].size() == 14);
  CHECK(json["alarms"]["entries"].size() == 1);
  CHECK(json["logs"]["entries"].size() == 1);
  CHECK(json["logs"]["entries"][0]["time"] == "00:00:01");

  const auto& d = json["drone_id"];
  CHECK(d["basic_id"]["uas_id"] == "DCDWCNS1AB12CD34EF56");
  CHECK(d["location"]["altitude_barometric"].get<double>() == doctest::Approx(45.2));
  CHECK(d["system"]["operator_altitude_geo"].get<double>() == doctest::Approx(45.0));
  CHECK(d["operator_id"]["operator_id"] == "CAAB1234567890");
  CHECK(d["self_id"]["description"] == "CNS-RPI training kit");
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: 大概率因为 `voltages_ext[0]` 那两行断言失败（前面 Task 7 只测了全 0 的情况，没测过"部分槽位非0"这种——如果实现本身已经对，这里其实会直接 PASS；如果因为之前实现有笔误漏了某个字段，这一步能暴露出来）。如果这一步意外全过，说明前面 13 个 Task 都已经正确实现，直接进 Step 3。

- [ ] **Step 3: 如果有失败项，回头检查对应 Task 的实现（通常是字段名拼写或者哨兵值判断条件写反），修好后重新跑到全绿**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -R json_serializer
```

Expected: 全部 PASS。

- [ ] **Step 4: 跑全部测试套件确认没有破坏其他模块**

```bash
ctest --test-dir build --output-on-failure
```

Expected: 全部 PASS（`mavlink_link`/`app_config`/`state_store`/`telemetry_decoder`/`extension_decoder`/`identity`/`json_serializer` 七个套件）。

- [ ] **Step 5: Commit**

```bash
git add tests/test_json_serializer.cpp
git commit -m "$(cat <<'EOF'
test: json_serializer补充全字段集成测试

前面每个Task都是单独测一组字段，这条补上全部字段同时有值的
集成测试，对应设计文档第11节的完整示例；顺便补上voltages_ext
"部分槽位非0"这种前面没覆盖到的哨兵值场景。

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review 记录

- **Spec 覆盖检查**：设计文档 11 个章节逐一核对——§1(背景/范围排除字段)→Task 5/12 的排除项；§2(接口/前置条件)→Task 1/2/3；§3(顶层结构/省略规则)→Task 3/10/11/12/14 的 `contains` 断言；§4(枚举字符串化范围)→Task 9/10 的 `ModuleStateToString`，其余官方枚举在每个 Task 里都是原始数字直接透传；§5(单位换算表)→逐条对应到 Task 4-8/12 的每个字段；§6(自定义字段key名)→Task 9；§7(drone_id/alarms/logs细节)→Task 11/12；§8(main.cpp集成)→Task 13；§9(测试范围)→每个 Task 都有对应测试，Task 1 补了 battery_status 按 id 独立存储的测试，Task 14 补了全字段集成测试；§10(全局约束)→写进了本计划的 Global Constraints 段；§11(完整示例)→Task 14 基本覆盖了示例里的字段组合。没有遗漏的章节。
- **占位符检查**：通读一遍，没有 "TODO"/"待定"/"类似 Task N" 这类占位描述，每个 Step 都是可以直接抄的完整代码。
- **类型一致性检查**：`Add*(nlohmann::json& telemetry, const state::TelemetryState& state)` 这个签名在 Task 4-9 里保持一致；`Add*(nlohmann::json& drone_id, const state::TelemetryState& state)` 在 Task 12 里也保持一致；`state.battery_status[0]/[1]` 这个下标用法从 Task 1 定下来之后，Task 7/14 都是同样的用法，没有出现命名不一致的问题。
- **发现的一处设计文档本身的不一致，已经按"忠实实现已批准设计"处理，未在本计划里擅自纠正**：设计文档的哨兵值清单里只列了 `sys_status.current_battery`/`battery.current_battery`/`battery.battery_remaining` 三个 `-1` 哨兵，没有列 `sys_status.battery_remaining`（这个官方字段的注释同样写着"-1: 未提供"）；`battery.temperature`/`pressure.temperature` 等字段的换算公式也没有为官方 `INT16_MAX`（未知温度）配哨兵规则。本计划严格按文档已列出的规则实现（`sys_status.battery_remaining` 直接透传，不加 null 判断），没有自作主张新增文档未提及的哨兵逻辑。如果这是设计疏漏，建议在下一轮设计评审里补充，而不是在实施阶段单方面改动已批准的规则。

---

**Plan complete and saved to `docs/superpowers/plans/2026-07-08-m4-json-payload.md`.**
