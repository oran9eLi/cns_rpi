# M3a：基础遥测解码 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 M2 已经跑通的完整 MAVLink 帧（`mavlink_message_t`）解码成 M3a 范围内 7 种标准消息（HEARTBEAT/GPS_RAW_INT/ATTITUDE/GLOBAL_POSITION_INT/SYS_STATUS/BATTERY_STATUS/SCALED_PRESSURE）的具体字段，写入一份线程安全的内部状态（`state_store`），main.cpp 打印出来做人工验证。

**Architecture:** 新增 `state/state_store`（一把大锁保护的 `TelemetryState`，7 个 `std::optional` 子字段存官方结构体原样）+ `protocol/telemetry_decoder`（`DecodeAndStore(msg, store)` 统一入口，按 msgid switch 分发到官方 decode 函数，不认识的 msgid 安静返回 false）。main.cpp 收到帧后调用 `DecodeAndStore`，成功就打印对应字段。

**Tech Stack:** C++23 / 官方 `mavlink/c_library_v2`（`src/mavlink/`，只读）/ doctest（单测）。

## Global Constraints

- C++23，`-Wall -Wextra` 零警告（已在 `CMakeLists.txt` 设置，不要改）。
- `state_store` 和 `telemetry_decoder` 这里**不用** `std::expected`——`state_store` 的操作（写内存、加锁）不会失败；`telemetry_decoder::DecodeAndStore` 返回 `bool` 表示"是否认识这个 msgid"，"不认识"是正常预期情况（M3a 范围外的消息类型），不是错误，用 `bool` 就够，不要包装成 `std::expected<void, SomeError>`。
- Doxygen 风格中文注释：文件头写职责/层级/依赖边界，公开函数写入参/返回值/失败语义（`docs/协作规则.md` §3）。
- 提交信息格式：`<type>: <简短中文说明>`。
- `src/mavlink/` 下的官方头文件只读，本计划任何任务都不修改它。
- 存官方 MAVLink 结构体原样（比如 `mavlink_gps_raw_int_t`），不做单位换算、不筛选字段——单位换算是 M4 `payload/json_serializer` 的事，这里不做。

---

### Task 1: `state/state_store` —— 线程安全的遥测状态存取

**Files:**
- Create: `src/state/state_store.hpp`
- Create: `src/state/state_store.cpp`
- Create: `tests/test_state_store.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct state::TelemetryState { std::optional<mavlink_heartbeat_t> heartbeat; std::optional<mavlink_gps_raw_int_t> gps_raw_int; std::optional<mavlink_attitude_t> attitude; std::optional<mavlink_global_position_int_t> global_position_int; std::optional<mavlink_sys_status_t> sys_status; std::optional<mavlink_battery_status_t> battery_status; std::optional<mavlink_scaled_pressure_t> scaled_pressure; };`
  - `class state::StateStore`：`void UpdateHeartbeat(const mavlink_heartbeat_t&)`、`void UpdateGpsRawInt(const mavlink_gps_raw_int_t&)`、`void UpdateAttitude(const mavlink_attitude_t&)`、`void UpdateGlobalPositionInt(const mavlink_global_position_int_t&)`、`void UpdateSysStatus(const mavlink_sys_status_t&)`、`void UpdateBatteryStatus(const mavlink_battery_status_t&)`、`void UpdateScaledPressure(const mavlink_scaled_pressure_t&)`、`TelemetryState Snapshot() const`。

- [ ] **Step 1: 写失败的测试 `tests/test_state_store.cpp`**

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "state/state_store.hpp"

TEST_CASE("初始状态所有字段都是nullopt") {
  state::StateStore store;

  auto snapshot = store.Snapshot();

  CHECK_FALSE(snapshot.heartbeat.has_value());
  CHECK_FALSE(snapshot.gps_raw_int.has_value());
  CHECK_FALSE(snapshot.attitude.has_value());
  CHECK_FALSE(snapshot.global_position_int.has_value());
  CHECK_FALSE(snapshot.sys_status.has_value());
  CHECK_FALSE(snapshot.battery_status.has_value());
  CHECK_FALSE(snapshot.scaled_pressure.has_value());
}

TEST_CASE("Update一个字段只影响那一个字段，其余仍是nullopt") {
  state::StateStore store;
  mavlink_heartbeat_t heartbeat{};
  heartbeat.type = 18;
  heartbeat.autopilot = 8;

  store.UpdateHeartbeat(heartbeat);
  auto snapshot = store.Snapshot();

  REQUIRE(snapshot.heartbeat.has_value());
  CHECK(snapshot.heartbeat->type == 18);
  CHECK(snapshot.heartbeat->autopilot == 8);
  CHECK_FALSE(snapshot.gps_raw_int.has_value());
  CHECK_FALSE(snapshot.attitude.has_value());
}

TEST_CASE("同一字段多次Update，Snapshot返回最新值") {
  state::StateStore store;
  mavlink_scaled_pressure_t first{};
  first.press_abs = 1000.0F;
  mavlink_scaled_pressure_t second{};
  second.press_abs = 1013.25F;

  store.UpdateScaledPressure(first);
  store.UpdateScaledPressure(second);
  auto snapshot = store.Snapshot();

  REQUIRE(snapshot.scaled_pressure.has_value());
  CHECK(snapshot.scaled_pressure->press_abs == doctest::Approx(1013.25F));
}
```

- [ ] **Step 2: 把测试加进 CMakeLists.txt，运行确认失败**

编辑 `CMakeLists.txt`，在 `add_test(NAME app_config COMMAND test_app_config)` 之后加：

```cmake
add_executable(test_state_store tests/test_state_store.cpp)
target_link_libraries(test_state_store PRIVATE cns_rpi_core)
add_test(NAME state_store COMMAND test_state_store)
```

Run: `cmake -B build -S . && cmake --build build`
Expected: 编译失败，报错找不到 `state/state_store.hpp`（`fatal error: state/state_store.hpp: No such file or directory`）。

- [ ] **Step 3: 写 `src/state/state_store.hpp`**

```cpp
#pragma once

/**
 * @file state_store.hpp
 * @brief 解码后的内部遥测状态，线程安全存取。
 *
 * @details
 * 只负责"存一份当前已知的最新遥测状态"，不关心谁写（protocol/telemetry_decoder
 * 的事）、谁读（V1: MQTT 发布，V2 预留: Qt 渲染/CV 管线）。存官方 MAVLink 结构体
 * 原样，不做单位换算/字段筛选——那是 payload/json_serializer（M4）的事。
 * 依赖边界：只依赖官方 mavlink/c_library_v2 头文件和标准库，不包含 uart/、mqtt/
 * 等模块头文件。
 */

#include <mutex>
#include <optional>

#include "common/mavlink.h"

namespace state {

/// 一份遥测快照：每个字段在对应消息从未被解码过之前是 std::nullopt。
struct TelemetryState {
  std::optional<mavlink_heartbeat_t> heartbeat;
  std::optional<mavlink_gps_raw_int_t> gps_raw_int;
  std::optional<mavlink_attitude_t> attitude;
  std::optional<mavlink_global_position_int_t> global_position_int;
  std::optional<mavlink_sys_status_t> sys_status;
  std::optional<mavlink_battery_status_t> battery_status;
  std::optional<mavlink_scaled_pressure_t> scaled_pressure;
};

/**
 * @brief 线程安全的 TelemetryState 存取器。
 * @details 一把锁保护整个状态（数据量小、更新频率最高约 1Hz，细粒度锁是过度
 * 设计）。`Update*()` 加锁写入对应字段；`Snapshot()` 加锁拷贝整个状态返回，
 * 调用方拿到独立副本，不持有内部锁，可以在任意线程安全地读。
 */
class StateStore {
 public:
  void UpdateHeartbeat(const mavlink_heartbeat_t& value);
  void UpdateGpsRawInt(const mavlink_gps_raw_int_t& value);
  void UpdateAttitude(const mavlink_attitude_t& value);
  void UpdateGlobalPositionInt(const mavlink_global_position_int_t& value);
  void UpdateSysStatus(const mavlink_sys_status_t& value);
  void UpdateBatteryStatus(const mavlink_battery_status_t& value);
  void UpdateScaledPressure(const mavlink_scaled_pressure_t& value);

  /// 加锁拷贝当前状态并返回，调用方拿到的是独立副本。
  TelemetryState Snapshot() const;

 private:
  mutable std::mutex mutex_;
  TelemetryState state_;
};

}  // namespace state
```

- [ ] **Step 4: 写 `src/state/state_store.cpp`**

```cpp
/**
 * @file state_store.cpp
 * @brief state_store.hpp 的实现。
 */

#include "state/state_store.hpp"

namespace state {

void StateStore::UpdateHeartbeat(const mavlink_heartbeat_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.heartbeat = value;
}

void StateStore::UpdateGpsRawInt(const mavlink_gps_raw_int_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.gps_raw_int = value;
}

void StateStore::UpdateAttitude(const mavlink_attitude_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.attitude = value;
}

void StateStore::UpdateGlobalPositionInt(const mavlink_global_position_int_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.global_position_int = value;
}

void StateStore::UpdateSysStatus(const mavlink_sys_status_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.sys_status = value;
}

void StateStore::UpdateBatteryStatus(const mavlink_battery_status_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.battery_status = value;
}

void StateStore::UpdateScaledPressure(const mavlink_scaled_pressure_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.scaled_pressure = value;
}

TelemetryState StateStore::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

}  // namespace state
```

- [ ] **Step 5: 把新源文件加进 `cns_rpi_core` 库**

编辑 `CMakeLists.txt`，把：

```cmake
add_library(cns_rpi_core
    src/uart/serial_port.cpp
    src/uart/mavlink_link.cpp
    src/config/app_config.cpp
)
```

改成：

```cmake
add_library(cns_rpi_core
    src/uart/serial_port.cpp
    src/uart/mavlink_link.cpp
    src/config/app_config.cpp
    src/state/state_store.cpp
)
```

- [ ] **Step 6: 运行测试，确认通过**

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: 编译零警告；`state_store` 这个 test 通过（3 个 `TEST_CASE` 全绿），`mavlink_link`/`app_config` 两个既有 test 仍然通过。

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/state/state_store.hpp src/state/state_store.cpp tests/test_state_store.cpp
git commit -m "$(cat <<'EOF'
feat: 新增线程安全遥测状态存取(state_store)
EOF
)"
```

---

### Task 2: `protocol/telemetry_decoder` —— M3a 7 种标准消息的解码入口

**Files:**
- Create: `src/protocol/telemetry_decoder.hpp`
- Create: `src/protocol/telemetry_decoder.cpp`
- Create: `tests/test_telemetry_decoder.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `state::StateStore`（Task 1）的全部 `Update*()` 方法、`state::TelemetryState`。
- Produces: `bool protocol::DecodeAndStore(const mavlink_message_t& msg, state::StateStore& store);`

- [ ] **Step 1: 写失败的测试 `tests/test_telemetry_decoder.cpp`**

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>

#include "common/mavlink.h"
#include "protocol/telemetry_decoder.hpp"

namespace {
constexpr std::uint8_t kSystemId = 1;
constexpr std::uint8_t kComponentId = 1;
}  // namespace

TEST_CASE("HEARTBEAT解码写入store") {
  mavlink_message_t msg{};
  mavlink_msg_heartbeat_pack(kSystemId, kComponentId, &msg, /*type=*/18, /*autopilot=*/8,
                             /*base_mode=*/0, /*custom_mode=*/0, /*system_status=*/4);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.heartbeat.has_value());
  CHECK(snapshot.heartbeat->type == 18);
  CHECK(snapshot.heartbeat->autopilot == 8);
  CHECK(snapshot.heartbeat->system_status == 4);
}

TEST_CASE("GPS_RAW_INT解码写入store") {
  mavlink_message_t msg{};
  mavlink_msg_gps_raw_int_pack(kSystemId, kComponentId, &msg,
                               /*time_usec=*/123456789ULL, /*fix_type=*/3,
                               /*lat=*/396890000, /*lon=*/1164050000, /*alt=*/50000,
                               /*eph=*/100, /*epv=*/150, /*vel=*/500, /*cog=*/9000,
                               /*satellites_visible=*/12, /*alt_ellipsoid=*/50500,
                               /*h_acc=*/1000, /*v_acc=*/2000, /*vel_acc=*/300,
                               /*hdg_acc=*/500, /*yaw=*/0);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.gps_raw_int.has_value());
  CHECK(snapshot.gps_raw_int->fix_type == 3);
  CHECK(snapshot.gps_raw_int->lat == 396890000);
  CHECK(snapshot.gps_raw_int->lon == 1164050000);
  CHECK(snapshot.gps_raw_int->satellites_visible == 12);
}

TEST_CASE("ATTITUDE解码写入store") {
  mavlink_message_t msg{};
  mavlink_msg_attitude_pack(kSystemId, kComponentId, &msg, /*time_boot_ms=*/1000,
                            /*roll=*/0.1F, /*pitch=*/0.2F, /*yaw=*/0.3F,
                            /*rollspeed=*/0.01F, /*pitchspeed=*/0.02F, /*yawspeed=*/0.03F);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.attitude.has_value());
  CHECK(snapshot.attitude->roll == doctest::Approx(0.1F));
  CHECK(snapshot.attitude->pitch == doctest::Approx(0.2F));
  CHECK(snapshot.attitude->yaw == doctest::Approx(0.3F));
}

TEST_CASE("GLOBAL_POSITION_INT解码写入store") {
  mavlink_message_t msg{};
  mavlink_msg_global_position_int_pack(kSystemId, kComponentId, &msg, /*time_boot_ms=*/1000,
                                       /*lat=*/396890000, /*lon=*/1164050000, /*alt=*/50000,
                                       /*relative_alt=*/10000, /*vx=*/100, /*vy=*/200,
                                       /*vz=*/-50, /*hdg=*/9000);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.global_position_int.has_value());
  CHECK(snapshot.global_position_int->lat == 396890000);
  CHECK(snapshot.global_position_int->relative_alt == 10000);
  CHECK(snapshot.global_position_int->hdg == 9000);
}

TEST_CASE("SYS_STATUS解码写入store") {
  mavlink_message_t msg{};
  mavlink_msg_sys_status_pack(kSystemId, kComponentId, &msg,
                              /*onboard_control_sensors_present=*/0x01,
                              /*onboard_control_sensors_enabled=*/0x01,
                              /*onboard_control_sensors_health=*/0x01,
                              /*load=*/300, /*voltage_battery=*/12600, /*current_battery=*/150,
                              /*battery_remaining=*/80, /*drop_rate_comm=*/0, /*errors_comm=*/0,
                              /*errors_count1=*/0, /*errors_count2=*/0, /*errors_count3=*/0,
                              /*errors_count4=*/0,
                              /*onboard_control_sensors_present_extended=*/0,
                              /*onboard_control_sensors_enabled_extended=*/0,
                              /*onboard_control_sensors_health_extended=*/0);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.sys_status.has_value());
  CHECK(snapshot.sys_status->voltage_battery == 12600);
  CHECK(snapshot.sys_status->battery_remaining == 80);
}

TEST_CASE("BATTERY_STATUS解码写入store") {
  mavlink_message_t msg{};
  std::uint16_t voltages[10] = {4200, 4180, 4190, 0, 0, 0, 0, 0, 0, 0};
  std::uint16_t voltages_ext[4] = {0, 0, 0, 0};
  mavlink_msg_battery_status_pack(kSystemId, kComponentId, &msg, /*id=*/0,
                                  /*battery_function=*/0, /*type=*/0, /*temperature=*/2500,
                                  voltages, /*current_battery=*/150, /*current_consumed=*/500,
                                  /*energy_consumed=*/1000, /*battery_remaining=*/80,
                                  /*time_remaining=*/3600, /*charge_state=*/0, voltages_ext,
                                  /*mode=*/0, /*fault_bitmask=*/0);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.battery_status.has_value());
  CHECK(snapshot.battery_status->voltages[0] == 4200);
  CHECK(snapshot.battery_status->battery_remaining == 80);
}

TEST_CASE("SCALED_PRESSURE解码写入store") {
  mavlink_message_t msg{};
  mavlink_msg_scaled_pressure_pack(kSystemId, kComponentId, &msg, /*time_boot_ms=*/1000,
                                   /*press_abs=*/1013.25F, /*press_diff=*/0.5F,
                                   /*temperature=*/2500, /*temperature_press_diff=*/2500);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.scaled_pressure.has_value());
  CHECK(snapshot.scaled_pressure->press_abs == doctest::Approx(1013.25F));
}

TEST_CASE("不认识的消息类型被安静忽略，不影响其他已有字段") {
  state::StateStore store;
  mavlink_message_t heartbeat_msg{};
  mavlink_msg_heartbeat_pack(kSystemId, kComponentId, &heartbeat_msg, /*type=*/18,
                             /*autopilot=*/8, /*base_mode=*/0, /*custom_mode=*/0,
                             /*system_status=*/4);
  protocol::DecodeAndStore(heartbeat_msg, store);

  mavlink_message_t statustext_msg{};
  mavlink_msg_statustext_pack(kSystemId, kComponentId, &statustext_msg, /*severity=*/6,
                              "test", /*id=*/0, /*chunk_seq=*/0);

  bool handled = protocol::DecodeAndStore(statustext_msg, store);

  CHECK_FALSE(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.heartbeat.has_value());
  CHECK(snapshot.heartbeat->type == 18);
  CHECK_FALSE(snapshot.gps_raw_int.has_value());
}
```

- [ ] **Step 2: 把测试加进 CMakeLists.txt，运行确认失败**

编辑 `CMakeLists.txt`，在 `add_test(NAME state_store COMMAND test_state_store)` 之后加：

```cmake
add_executable(test_telemetry_decoder tests/test_telemetry_decoder.cpp)
target_link_libraries(test_telemetry_decoder PRIVATE cns_rpi_core)
add_test(NAME telemetry_decoder COMMAND test_telemetry_decoder)
```

Run: `cmake -B build -S . && cmake --build build`
Expected: 编译失败，找不到 `protocol/telemetry_decoder.hpp`。

- [ ] **Step 3: 写 `src/protocol/telemetry_decoder.hpp`**

```cpp
#pragma once

/**
 * @file telemetry_decoder.hpp
 * @brief M3a 范围内 7 种标准 MAVLink 消息的解码入口。
 *
 * @details
 * 只负责"认出 M3a 关心的 7 种消息、调官方 decode 函数、写入 state_store"，
 * 不关心帧是从哪来的（uart/mavlink_link 的事），也不关心解码后数据被谁读、
 * 怎么用（state/、mqtt/ 等下游模块的事）。M3a 之外的消息类型（扩展帧/身份帧/
 * 其他）一律安静忽略，不是错误。
 * 依赖边界：依赖 state/state_store.hpp 和官方 mavlink/c_library_v2 头文件，
 * 不包含 uart/、mqtt/ 等模块头文件。
 */

#include "common/mavlink.h"
#include "state/state_store.hpp"

namespace protocol {

/**
 * @brief 尝试把 msg 解码成 M3a 范围内的 7 种标准消息之一并写入 store。
 * @param msg 已经通过 CRC 校验的完整 MAVLink 帧。
 * @param store 解码结果写入的目标状态存储。
 * @return 是本函数认识的消息类型（成功解码并写入 store）返回 true；
 *         不认识的 msgid 返回 false，不写入、不报错。
 */
bool DecodeAndStore(const mavlink_message_t& msg, state::StateStore& store);

}  // namespace protocol
```

- [ ] **Step 4: 写 `src/protocol/telemetry_decoder.cpp`**

```cpp
/**
 * @file telemetry_decoder.cpp
 * @brief telemetry_decoder.hpp 的实现。
 */

#include "protocol/telemetry_decoder.hpp"

namespace protocol {

bool DecodeAndStore(const mavlink_message_t& msg, state::StateStore& store) {
  switch (msg.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT: {
      mavlink_heartbeat_t decoded{};
      mavlink_msg_heartbeat_decode(&msg, &decoded);
      store.UpdateHeartbeat(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
      mavlink_gps_raw_int_t decoded{};
      mavlink_msg_gps_raw_int_decode(&msg, &decoded);
      store.UpdateGpsRawInt(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_ATTITUDE: {
      mavlink_attitude_t decoded{};
      mavlink_msg_attitude_decode(&msg, &decoded);
      store.UpdateAttitude(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
      mavlink_global_position_int_t decoded{};
      mavlink_msg_global_position_int_decode(&msg, &decoded);
      store.UpdateGlobalPositionInt(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_SYS_STATUS: {
      mavlink_sys_status_t decoded{};
      mavlink_msg_sys_status_decode(&msg, &decoded);
      store.UpdateSysStatus(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_BATTERY_STATUS: {
      mavlink_battery_status_t decoded{};
      mavlink_msg_battery_status_decode(&msg, &decoded);
      store.UpdateBatteryStatus(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_SCALED_PRESSURE: {
      mavlink_scaled_pressure_t decoded{};
      mavlink_msg_scaled_pressure_decode(&msg, &decoded);
      store.UpdateScaledPressure(decoded);
      return true;
    }
    default:
      return false;
  }
}

}  // namespace protocol
```

- [ ] **Step 5: 把新源文件加进 `cns_rpi_core` 库**

编辑 `CMakeLists.txt`，把：

```cmake
add_library(cns_rpi_core
    src/uart/serial_port.cpp
    src/uart/mavlink_link.cpp
    src/config/app_config.cpp
    src/state/state_store.cpp
)
```

改成：

```cmake
add_library(cns_rpi_core
    src/uart/serial_port.cpp
    src/uart/mavlink_link.cpp
    src/config/app_config.cpp
    src/state/state_store.cpp
    src/protocol/telemetry_decoder.cpp
)
```

- [ ] **Step 6: 运行测试，确认全部通过**

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: 编译零警告；`telemetry_decoder` 这个 test 通过（8 个 `TEST_CASE` 全绿：7 种消息 + 1 个边界测试），其余既有 test（`mavlink_link`/`app_config`/`state_store`）仍然通过。

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/protocol/telemetry_decoder.hpp src/protocol/telemetry_decoder.cpp tests/test_telemetry_decoder.cpp
git commit -m "$(cat <<'EOF'
feat: 新增M3a基础遥测解码(telemetry_decoder)
EOF
)"
```

---

### Task 3: `main.cpp` —— 接入遥测解码

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `state::StateStore`/`state::TelemetryState`（Task 1）、`protocol::DecodeAndStore`（Task 2）。

这一步是集成，不是新的可单测单元（跟 M2 Task 5 一样：main.cpp 是组合根，靠编译零警告 + 真机人工验证）。

- [ ] **Step 1: 改写 `src/main.cpp`**

```cpp
/**
 * @file main.cpp
 * @brief 程序入口，组合根。
 *
 * @details
 * M3a 阶段在 M2 双向收发闭环基础上接入遥测解码：
 * 收到帧 -> protocol::DecodeAndStore 写入 state::StateStore -> 打印解码后的
 * 有意义字段做人工验证。不接 MQTT（M5 的事），不处理扩展帧/身份帧（M3b/M3c 的事）。
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "common/mavlink.h"
#include "config/app_config.hpp"
#include "protocol/telemetry_decoder.hpp"
#include "state/state_store.hpp"
#include "uart/mavlink_link.hpp"

namespace {

constexpr std::uint8_t kSystemId = 1;
constexpr std::uint8_t kComponentId = MAV_COMP_ID_ONBOARD_COMPUTER;
constexpr auto kHeartbeatInterval = std::chrono::seconds(1);

/// RPi 自己的 HEARTBEAT：它不是飞控，所以 autopilot=MAV_AUTOPILOT_INVALID。
mavlink_message_t BuildHeartbeat() {
  mavlink_message_t msg{};
  mavlink_msg_heartbeat_pack(kSystemId, kComponentId, &msg, MAV_TYPE_ONBOARD_CONTROLLER,
                              MAV_AUTOPILOT_INVALID, /*base_mode=*/0, /*custom_mode=*/0,
                              MAV_STATE_ACTIVE);
  return msg;
}

/// 按刚解出来的这条帧的 msgid，打印 state_store 里对应字段的最新值——
/// 只是给人看的调试日志，不是解码逻辑本身（解码逻辑在 protocol::DecodeAndStore 里）。
void LogTelemetry(std::uint32_t msgid, const state::TelemetryState& snapshot) {
  switch (msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT:
      if (snapshot.heartbeat) {
        std::cout << "HEARTBEAT: type=" << static_cast<int>(snapshot.heartbeat->type)
                  << " system_status=" << static_cast<int>(snapshot.heartbeat->system_status)
                  << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_GPS_RAW_INT:
      if (snapshot.gps_raw_int) {
        std::cout << "GPS_RAW_INT: fix_type=" << static_cast<int>(snapshot.gps_raw_int->fix_type)
                  << " lat=" << snapshot.gps_raw_int->lat
                  << " lon=" << snapshot.gps_raw_int->lon << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_ATTITUDE:
      if (snapshot.attitude) {
        std::cout << "ATTITUDE: roll=" << snapshot.attitude->roll
                  << " pitch=" << snapshot.attitude->pitch
                  << " yaw=" << snapshot.attitude->yaw << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
      if (snapshot.global_position_int) {
        std::cout << "GLOBAL_POSITION_INT: lat=" << snapshot.global_position_int->lat
                  << " lon=" << snapshot.global_position_int->lon
                  << " relative_alt=" << snapshot.global_position_int->relative_alt << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_SYS_STATUS:
      if (snapshot.sys_status) {
        std::cout << "SYS_STATUS: voltage_battery=" << snapshot.sys_status->voltage_battery
                  << " battery_remaining="
                  << static_cast<int>(snapshot.sys_status->battery_remaining) << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_BATTERY_STATUS:
      if (snapshot.battery_status) {
        std::cout << "BATTERY_STATUS: voltages[0]=" << snapshot.battery_status->voltages[0]
                  << " battery_remaining="
                  << static_cast<int>(snapshot.battery_status->battery_remaining) << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_SCALED_PRESSURE:
      if (snapshot.scaled_pressure) {
        std::cout << "SCALED_PRESSURE: press_abs=" << snapshot.scaled_pressure->press_abs
                  << " temperature=" << snapshot.scaled_pressure->temperature << std::endl;
      }
      break;
    default:
      break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string config_path = argc > 1 ? argv[1] : "config/config.json";

  auto app_config = config::LoadAppConfig(config_path);
  if (!app_config) {
    std::cerr << "读取配置失败: " << config_path << "\n";
    return EXIT_FAILURE;
  }

  auto link = uart::MavlinkLink::Open(app_config->serial.device, app_config->serial.baud);
  if (!link) {
    std::cerr << "打开串口失败: " << app_config->serial.device << "\n";
    return EXIT_FAILURE;
  }

  std::cout << "cns_rpi M3a 启动，串口=" << app_config->serial.device
            << " 波特率=" << app_config->serial.baud << std::endl;

  state::StateStore state_store;
  auto last_heartbeat = std::chrono::steady_clock::now();
  while (true) {
    if (auto msg = link->ReceiveMessage()) {
      if (protocol::DecodeAndStore(*msg, state_store)) {
        LogTelemetry(msg->msgid, state_store.Snapshot());
      }
    }

    auto now = std::chrono::steady_clock::now();
    if (now - last_heartbeat >= kHeartbeatInterval) {
      mavlink_message_t heartbeat = BuildHeartbeat();
      link->SendMessage(heartbeat);
      last_heartbeat = now;
    }
  }
}
```

- [ ] **Step 2: 验证编译零警告，既有测试不受影响**

Run: `cmake -B build -S . && cmake --build build 2>&1 | tee /tmp/build.log && grep -i warning /tmp/build.log && ctest --test-dir build --output-on-failure`
Expected: 构建成功，无警告输出；全部 4 个 test（`mavlink_link`/`app_config`/`state_store`/`telemetry_decoder`）通过。

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
feat: main.cpp接入M3a遥测解码
EOF
)"
```

---

### Task 4: 文档同步 —— V1设计文档.md 标记 M3a 已实现

**Files:**
- Modify: `docs/V1设计文档.md`

- [ ] **Step 1: 更新 §10 M3a 里程碑那一条**

把：

```
- **M3a 基础遥测解码**：HEARTBEAT + GPS_RAW_INT/ATTITUDE/GLOBAL_POSITION_INT/SYS_STATUS/BATTERY_STATUS/SCALED_PRESSURE，写入 `state_store`
```

改成：

```
- **M3a 基础遥测解码**：HEARTBEAT + GPS_RAW_INT/ATTITUDE/GLOBAL_POSITION_INT/SYS_STATUS/BATTERY_STATUS/SCALED_PRESSURE，写入 `state_store`——已实现，见 `docs/superpowers/plans/2026-07-03-m3a-telemetry-decode.md`
```

- [ ] **Step 2: 检查改动只涉及这一处**

Run: `git diff docs/V1设计文档.md`
Expected: diff 只包含上面这一处替换。

- [ ] **Step 3: Commit**

```bash
git add docs/V1设计文档.md
git commit -m "$(cat <<'EOF'
docs: M3a落地后标记里程碑已实现
EOF
)"
```
