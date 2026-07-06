# M3b：扩展帧解码 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `NAMED_VALUE_INT`（`MODSTAT0`/`MODSTAT1`/`BAT2STAT`/`MOTORPWM`/`GNSS_SAT`/`ENVHUM`）和 `TUNNEL`（告警表 `0x8001`/日志增量 `0x8002`）这两类扩展帧解码成有名字的字段，写入 `state_store`，main.cpp 打印出来做人工验证。

**Architecture:** `state_store` 新增 7 个扩展字段（`module_status` 由 `MODSTAT0`/`MODSTAT1` 各自只覆写一半后合并成 14 元素数组，其余 6 个各自整体覆盖）+ 新增 `protocol/extension_decoder`（`DecodeExtensionAndStore(msg, store)`，对 `NAMED_VALUE_INT` 按 `name` 二次分派做位打包解码，对 `TUNNEL` 按 `payload_type` 二次分派做字节级解码，均只做结构提取不做单位换算）。main.cpp 依次尝试 `telemetry_decoder`/`extension_decoder` 两个解码器。

**Tech Stack:** C++23 / 官方 `mavlink/c_library_v2`（`src/mavlink/`，只读）/ doctest（单测）。

## Global Constraints

- C++23，`-Wall -Wextra` 零警告（已在 `CMakeLists.txt` 设置，不要改）。
- 不用 `std::expected`——`DecodeExtensionAndStore` 返回 `bool` 表示"是否认识/成功解码这条帧"，"不认识的 name/payload_type"和"payload_length 不足以容纳表头的畸形帧"都归为"不认识"，安静返回 `false`、不写入 store，不是错误，跟 `telemetry_decoder` 的语义一致。
- 拆包但不换算：`NAMED_VALUE_INT` 的子字段保持 wire 上的原始整数刻度（湿度存 535 不存 53.5，电压存 12600mV 不存 12.6V）；单位换算留给 M4 `payload/json_serializer`。
- Doxygen 风格中文注释：文件头写职责/层级/依赖边界，公开函数写入参/返回值/失败语义（`docs/协作规则.md` §3）。
- 提交信息格式：`<type>: <简短中文说明>`。
- `src/mavlink/` 下的官方头文件只读，本计划任何任务都不修改它。
- 字段字典依据：`docs/V1设计文档.md` §4.1（`NAMED_VALUE_INT`）/§4.2（`TUNNEL`）。设计依据：`docs/superpowers/specs/2026-07-06-m3b-extension-decode-design.md`。
- 函数命名：扩展帧解码入口叫 `protocol::DecodeExtensionAndStore`，不能跟 `telemetry_decoder.hpp` 已有的 `protocol::DecodeAndStore` 同名同签名（两者都在 `protocol` 命名空间下，同名会导致链接期重复定义）。

---

### Task 1: `state/state_store` —— 新增扩展遥测字段

**Files:**
- Modify: `src/state/state_store.hpp`
- Modify: `src/state/state_store.cpp`
- Modify: `tests/test_state_store.cpp`

**Interfaces:**
- Produces（新增，不改动 M3a 已有的 7 个字段/方法）：
  - `constexpr std::size_t state::kModuleCount = 14;`
  - `struct state::Battery2Status { std::uint16_t voltage_mv; std::uint8_t percent; bool low_voltage; };`
  - `struct state::MotorPwm { std::array<std::uint8_t, 4> duty_percent; };`
  - `struct state::GnssSat { std::uint8_t gps_visible; std::uint8_t beidou_visible; std::uint8_t gps_used; std::uint8_t beidou_used; };`
  - `struct state::EnvHumidity { std::uint16_t relative_humidity_x10; };`
  - `struct state::AlarmEntry { std::uint8_t source_id; std::uint16_t fault_code; std::uint8_t severity; bool active; std::uint16_t age_s; };`
  - `struct state::AlarmTable { std::uint8_t ver; std::array<AlarmEntry, 14> entries; std::size_t active_count; };`
  - `struct state::LogEntry { std::uint16_t sequence; std::uint16_t message_id; std::array<std::uint8_t, 3> time_hhmmss; std::uint8_t severity; };`
  - `struct state::MessageLog { std::uint16_t latest_seq; std::array<LogEntry, 9> entries; std::size_t count; };`
  - `TelemetryState` 新增字段：`module_status`（`std::optional<std::array<std::uint8_t, kModuleCount>>`）、`battery2_status`、`motor_pwm`、`gnss_sat`、`env_humidity`、`alarm_table`、`message_log`（均 `std::optional<对应类型>`）。
  - `StateStore` 新增方法：`void UpdateModStatusLow(const std::array<std::uint8_t, 8>&)`、`void UpdateModStatusHigh(const std::array<std::uint8_t, 6>&)`、`void UpdateBattery2Status(const Battery2Status&)`、`void UpdateMotorPwm(const MotorPwm&)`、`void UpdateGnssSat(const GnssSat&)`、`void UpdateEnvHumidity(const EnvHumidity&)`、`void UpdateAlarmTable(const AlarmTable&)`、`void UpdateMessageLog(const MessageLog&)`。

- [ ] **Step 1: 在 `tests/test_state_store.cpp` 末尾追加失败的测试**

先把文件顶部的 include 从：

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "state/state_store.hpp"
```

改成：

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>

#include "state/state_store.hpp"
```

然后在文件最后一个 `TEST_CASE`（`"同一字段多次Update，Snapshot返回最新值"`）之后追加：

```cpp

TEST_CASE("UpdateModStatusLow只影响0-7号模块,UpdateModStatusHigh只影响8-13号,互不覆盖") {
  state::StateStore store;
  std::array<std::uint8_t, 8> low{0, 1, 2, 3, 4, 5, 6, 7};
  std::array<std::uint8_t, 6> high{0, 1, 2, 3, 4, 5};

  store.UpdateModStatusLow(low);
  auto after_low = store.Snapshot();
  REQUIRE(after_low.module_status.has_value());
  CHECK((*after_low.module_status)[0] == 0);
  CHECK((*after_low.module_status)[7] == 7);
  CHECK((*after_low.module_status)[8] == 0);  // MODSTAT1还没到，零初始化占位

  store.UpdateModStatusHigh(high);
  auto after_high = store.Snapshot();
  REQUIRE(after_high.module_status.has_value());
  CHECK((*after_high.module_status)[0] == 0);
  CHECK((*after_high.module_status)[7] == 7);  // 之前0-7的值没被UpdateModStatusHigh覆盖
  CHECK((*after_high.module_status)[8] == 0);
  CHECK((*after_high.module_status)[13] == 5);
}

TEST_CASE("扩展遥测字段的Update各自独立,不影响其他字段") {
  state::StateStore store;
  state::Battery2Status bat2{12600, 80, false};
  state::MotorPwm pwm{{10, 20, 30, 40}};
  state::GnssSat sat{12, 8, 10, 6};
  state::EnvHumidity hum{535};

  store.UpdateBattery2Status(bat2);
  store.UpdateMotorPwm(pwm);
  store.UpdateGnssSat(sat);
  store.UpdateEnvHumidity(hum);
  auto snapshot = store.Snapshot();

  REQUIRE(snapshot.battery2_status.has_value());
  CHECK(snapshot.battery2_status->voltage_mv == 12600);
  CHECK(snapshot.battery2_status->percent == 80);
  CHECK_FALSE(snapshot.battery2_status->low_voltage);

  REQUIRE(snapshot.motor_pwm.has_value());
  CHECK(snapshot.motor_pwm->duty_percent[3] == 40);

  REQUIRE(snapshot.gnss_sat.has_value());
  CHECK(snapshot.gnss_sat->beidou_used == 6);

  REQUIRE(snapshot.env_humidity.has_value());
  CHECK(snapshot.env_humidity->relative_humidity_x10 == 535);

  CHECK_FALSE(snapshot.alarm_table.has_value());
  CHECK_FALSE(snapshot.message_log.has_value());
}
```

- [ ] **Step 2: 运行测试，确认编译失败**

Run: `cmake -B build -S . && cmake --build build`
Expected: 编译失败，报错找不到 `UpdateModStatusLow`/`battery2_status` 等新符号。

- [ ] **Step 3: 修改 `src/state/state_store.hpp`**

把文件顶部 include 从：

```cpp
#include <mutex>
#include <optional>

#include "common/mavlink.h"
```

改成：

```cpp
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include "common/mavlink.h"
```

在 `namespace state {` 之后、`struct TelemetryState` 之前插入：

```cpp

/// 固件端模块总数（`Px4Lite_ModuleId_t`，见 V1设计文档.md §4.1），
/// MODSTAT0 覆盖 0-7 号，MODSTAT1 覆盖 8-13 号。
constexpr std::size_t kModuleCount = 14;

/// BAT2STAT 拆包结果：电压(mV)/电量(%)/低电压标志，原始刻度，不做单位换算。
struct Battery2Status {
  std::uint16_t voltage_mv;
  std::uint8_t percent;
  bool low_voltage;
};

/// MOTORPWM 拆包结果：最多 4 个电机的占空比(%)。
struct MotorPwm {
  std::array<std::uint8_t, 4> duty_percent;
};

/// GNSS_SAT 拆包结果：GPS/北斗可见数与使用数。
struct GnssSat {
  std::uint8_t gps_visible;
  std::uint8_t beidou_visible;
  std::uint8_t gps_used;
  std::uint8_t beidou_used;
};

/// ENVHUM 拆包结果：相对湿度 x10（原始刻度，535 表示 53.5%，不做单位换算）。
struct EnvHumidity {
  std::uint16_t relative_humidity_x10;
};

/// TUNNEL 告警表(payload_type=0x8001)单行。
struct AlarmEntry {
  std::uint8_t source_id;
  std::uint16_t fault_code;
  std::uint8_t severity;
  bool active;
  std::uint16_t age_s;
};

/// TUNNEL 告警表(payload_type=0x8001)整帧，最多 14 行。
struct AlarmTable {
  std::uint8_t ver;
  std::array<AlarmEntry, 14> entries;
  std::size_t active_count;  ///< entries 中前 active_count 项有效，其余是默认值。
};

/// TUNNEL 日志增量(payload_type=0x8002)单条。
struct LogEntry {
  std::uint16_t sequence;
  std::uint16_t message_id;
  std::array<std::uint8_t, 3> time_hhmmss;
  std::uint8_t severity;
};

/// TUNNEL 日志增量(payload_type=0x8002)整帧，最多 9 条；count=0 表示只有心跳。
struct MessageLog {
  std::uint16_t latest_seq;
  std::array<LogEntry, 9> entries;
  std::size_t count;  ///< entries 中前 count 项有效，其余是默认值。
};
```

在 `struct TelemetryState` 内、`scaled_pressure` 字段之后插入：

```cpp
  /// 14 个模块的状态(0-6，含义见 V1设计文档.md §4.1"模块状态枚举")，
  /// MODSTAT0 只写 0-7 号，MODSTAT1 只写 8-13 号，两条帧合并成一份数据。
  std::optional<std::array<std::uint8_t, kModuleCount>> module_status;
  std::optional<Battery2Status> battery2_status;
  std::optional<MotorPwm> motor_pwm;
  std::optional<GnssSat> gnss_sat;
  std::optional<EnvHumidity> env_humidity;
  std::optional<AlarmTable> alarm_table;
  std::optional<MessageLog> message_log;
```

在 `class StateStore` 的 `public:` 区、`UpdateScaledPressure` 之后插入：

```cpp
  /// 只写 module_status 的 0-7 号元素(来自 MODSTAT0)。若 module_status 之前
  /// 还没有值(两条帧都还没收到过)，先把整个 14 元素数组零初始化，
  /// 再写入自己负责的这一半；8-13 号元素(若已收到过 MODSTAT1)保持不变。
  void UpdateModStatusLow(const std::array<std::uint8_t, 8>& modules0to7);
  /// 只写 module_status 的 8-13 号元素(来自 MODSTAT1)，语义同上。
  void UpdateModStatusHigh(const std::array<std::uint8_t, 6>& modules8to13);
  void UpdateBattery2Status(const Battery2Status& value);
  void UpdateMotorPwm(const MotorPwm& value);
  void UpdateGnssSat(const GnssSat& value);
  void UpdateEnvHumidity(const EnvHumidity& value);
  void UpdateAlarmTable(const AlarmTable& value);
  void UpdateMessageLog(const MessageLog& value);
```

- [ ] **Step 4: 修改 `src/state/state_store.cpp`**

在 `UpdateScaledPressure` 实现之后、`TelemetryState StateStore::Snapshot()` 之前插入：

```cpp
void StateStore::UpdateModStatusLow(const std::array<std::uint8_t, 8>& modules0to7) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!state_.module_status.has_value()) {
    state_.module_status = std::array<std::uint8_t, kModuleCount>{};
  }
  for (std::size_t i = 0; i < modules0to7.size(); ++i) {
    (*state_.module_status)[i] = modules0to7[i];
  }
}

void StateStore::UpdateModStatusHigh(const std::array<std::uint8_t, 6>& modules8to13) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!state_.module_status.has_value()) {
    state_.module_status = std::array<std::uint8_t, kModuleCount>{};
  }
  for (std::size_t i = 0; i < modules8to13.size(); ++i) {
    (*state_.module_status)[8 + i] = modules8to13[i];
  }
}

void StateStore::UpdateBattery2Status(const Battery2Status& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.battery2_status = value;
}

void StateStore::UpdateMotorPwm(const MotorPwm& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.motor_pwm = value;
}

void StateStore::UpdateGnssSat(const GnssSat& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.gnss_sat = value;
}

void StateStore::UpdateEnvHumidity(const EnvHumidity& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.env_humidity = value;
}

void StateStore::UpdateAlarmTable(const AlarmTable& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.alarm_table = value;
}

void StateStore::UpdateMessageLog(const MessageLog& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.message_log = value;
}
```

- [ ] **Step 5: 运行测试，确认全部通过**

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: 编译零警告；`state_store` 这个 test 通过（5 个 `TEST_CASE` 全绿：3 个 M3a 既有 + 2 个本任务新增），其余既有 test 不受影响。

- [ ] **Step 6: Commit**

```bash
git add src/state/state_store.hpp src/state/state_store.cpp tests/test_state_store.cpp
git commit -m "$(cat <<'EOF'
feat: state_store新增M3b扩展遥测字段
EOF
)"
```

---

### Task 2: `protocol/extension_decoder` —— NAMED_VALUE_INT 位打包解码

**Files:**
- Create: `src/protocol/extension_decoder.hpp`
- Create: `src/protocol/extension_decoder.cpp`
- Create: `tests/test_extension_decoder.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `state::StateStore`（Task 1）的 `UpdateModStatusLow`/`UpdateModStatusHigh`/`UpdateBattery2Status`/`UpdateMotorPwm`/`UpdateGnssSat`/`UpdateEnvHumidity`。
- Produces: `bool protocol::DecodeExtensionAndStore(const mavlink_message_t& msg, state::StateStore& store);`（本任务先实现 `NAMED_VALUE_INT` 分支，`TUNNEL` 分支和 `default` 一律 `return false`，Task 3 再补 `TUNNEL`）。

- [ ] **Step 1: 写失败的测试 `tests/test_extension_decoder.cpp`**

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>

#include "common/mavlink.h"
#include "protocol/extension_decoder.hpp"

namespace {
constexpr std::uint8_t kSystemId = 1;
constexpr std::uint8_t kComponentId = 1;

mavlink_message_t PackNamedValueInt(const char* name, std::int32_t value) {
  mavlink_message_t msg{};
  mavlink_msg_named_value_int_pack(kSystemId, kComponentId, &msg, /*time_boot_ms=*/1000, name,
                                    value);
  return msg;
}
}  // namespace

TEST_CASE("MODSTAT0解码写入module_status的0-7号,8-13号保持零初始化") {
  constexpr std::int32_t kValue = static_cast<std::int32_t>(
      (0u << 0) | (1u << 4) | (2u << 8) | (3u << 12) | (4u << 16) | (5u << 20) | (6u << 24) |
      (7u << 28));
  mavlink_message_t msg = PackNamedValueInt("MODSTAT0", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.module_status.has_value());
  CHECK((*snapshot.module_status)[0] == 0);
  CHECK((*snapshot.module_status)[3] == 3);
  CHECK((*snapshot.module_status)[7] == 7);
  CHECK((*snapshot.module_status)[8] == 0);
}

TEST_CASE("MODSTAT1解码写入module_status的8-13号,不影响先前的0-7号") {
  constexpr std::int32_t kLowValue = static_cast<std::int32_t>(
      (0u << 0) | (1u << 4) | (2u << 8) | (3u << 12) | (4u << 16) | (5u << 20) | (6u << 24) |
      (7u << 28));
  constexpr std::int32_t kHighValue = static_cast<std::int32_t>(
      (0u << 0) | (1u << 4) | (2u << 8) | (3u << 12) | (4u << 16) | (5u << 20));
  state::StateStore store;
  protocol::DecodeExtensionAndStore(PackNamedValueInt("MODSTAT0", kLowValue), store);

  bool handled = protocol::DecodeExtensionAndStore(PackNamedValueInt("MODSTAT1", kHighValue), store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.module_status.has_value());
  CHECK((*snapshot.module_status)[7] == 7);   // 0-7号仍是MODSTAT0写入的值
  CHECK((*snapshot.module_status)[8] == 0);
  CHECK((*snapshot.module_status)[13] == 5);
}

TEST_CASE("BAT2STAT解码拆出电压/电量/低电压标志") {
  constexpr std::int32_t kValue = static_cast<std::int32_t>(10500u | (15u << 16) | (1u << 24));
  mavlink_message_t msg = PackNamedValueInt("BAT2STAT", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.battery2_status.has_value());
  CHECK(snapshot.battery2_status->voltage_mv == 10500);
  CHECK(snapshot.battery2_status->percent == 15);
  CHECK(snapshot.battery2_status->low_voltage);
}

TEST_CASE("MOTORPWM解码拆出4个电机占空比") {
  constexpr std::int32_t kValue =
      static_cast<std::int32_t>(10u | (20u << 8) | (30u << 16) | (40u << 24));
  mavlink_message_t msg = PackNamedValueInt("MOTORPWM", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.motor_pwm.has_value());
  CHECK(snapshot.motor_pwm->duty_percent[0] == 10);
  CHECK(snapshot.motor_pwm->duty_percent[1] == 20);
  CHECK(snapshot.motor_pwm->duty_percent[2] == 30);
  CHECK(snapshot.motor_pwm->duty_percent[3] == 40);
}

TEST_CASE("GNSS_SAT解码拆出GPS/北斗可见数与使用数") {
  constexpr std::int32_t kValue =
      static_cast<std::int32_t>(12u | (8u << 8) | (10u << 16) | (6u << 24));
  mavlink_message_t msg = PackNamedValueInt("GNSS_SAT", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.gnss_sat.has_value());
  CHECK(snapshot.gnss_sat->gps_visible == 12);
  CHECK(snapshot.gnss_sat->beidou_visible == 8);
  CHECK(snapshot.gnss_sat->gps_used == 10);
  CHECK(snapshot.gnss_sat->beidou_used == 6);
}

TEST_CASE("ENVHUM解码保持原始x10刻度,不做单位换算") {
  mavlink_message_t msg = PackNamedValueInt("ENVHUM", /*value=*/535);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.env_humidity.has_value());
  CHECK(snapshot.env_humidity->relative_humidity_x10 == 535);
}

TEST_CASE("不认识的NAMED_VALUE_INT名字被安静忽略") {
  mavlink_message_t msg = PackNamedValueInt("FOOBAR", /*value=*/123);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK_FALSE(handled);
  auto snapshot = store.Snapshot();
  CHECK_FALSE(snapshot.env_humidity.has_value());
}

TEST_CASE("不认识的消息类型被安静忽略") {
  mavlink_message_t msg{};
  mavlink_msg_heartbeat_pack(kSystemId, kComponentId, &msg, /*type=*/18, /*autopilot=*/8,
                             /*base_mode=*/0, /*custom_mode=*/0, /*system_status=*/4);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK_FALSE(handled);
}
```

- [ ] **Step 2: 把测试加进 `CMakeLists.txt`，运行确认失败**

编辑 `CMakeLists.txt`，把：

```cmake
add_library(cns_rpi_core
    src/uart/serial_port.cpp
    src/uart/mavlink_link.cpp
    src/config/app_config.cpp
    src/state/state_store.cpp
    src/protocol/telemetry_decoder.cpp
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
    src/protocol/extension_decoder.cpp
)
```

在 `add_test(NAME telemetry_decoder COMMAND test_telemetry_decoder)` 之后加：

```cmake
add_executable(test_extension_decoder tests/test_extension_decoder.cpp)
target_link_libraries(test_extension_decoder PRIVATE cns_rpi_core)
add_test(NAME extension_decoder COMMAND test_extension_decoder)
```

Run: `cmake -B build -S . && cmake --build build`
Expected: 编译失败，找不到 `protocol/extension_decoder.hpp`。

- [ ] **Step 3: 写 `src/protocol/extension_decoder.hpp`**

```cpp
#pragma once

/**
 * @file extension_decoder.hpp
 * @brief M3b 范围内 NAMED_VALUE_INT/TUNNEL 扩展帧的解码入口。
 *
 * @details
 * 只负责"认出 M3b 关心的扩展帧语义(MODSTAT0/MODSTAT1/BAT2STAT/MOTORPWM/
 * GNSS_SAT/ENVHUM 六种 NAMED_VALUE_INT + 告警表/日志增量两种 TUNNEL)、拆包、
 * 写入 state_store"，不做单位换算(留给 M4 payload/json_serializer)，
 * 不关心帧从哪来(uart/mavlink_link 的事)、被谁读(state/ 下游消费者的事)。
 * M3b 范围外的消息类型/name/payload_type，以及 payload_length 不足以容纳
 * 表头的畸形 TUNNEL 帧，一律安静忽略，不是错误。
 * 依赖边界：依赖 state/state_store.hpp 和官方 mavlink/c_library_v2 头文件，
 * 不包含 uart/、mqtt/ 等模块头文件。跟 telemetry_decoder.hpp 是同一层级的
 * 平行模块，各自处理不同的消息范围，函数名不同(DecodeExtensionAndStore vs
 * DecodeAndStore)以避免 protocol 命名空间下的重复定义。
 */

#include "common/mavlink.h"
#include "state/state_store.hpp"

namespace protocol {

/**
 * @brief 尝试把 msg 解码成 M3b 范围内的扩展帧之一并写入 store。
 * @param msg 已经通过 CRC 校验的完整 MAVLink 帧。
 * @param store 解码结果写入的目标状态存储。
 * @return 是本函数认识的消息类型/name/payload_type(成功解码并写入 store)
 *         返回 true；不认识的一律返回 false，不写入、不报错(含 payload_length
 *         不足以容纳表头的畸形 TUNNEL 帧)。
 */
bool DecodeExtensionAndStore(const mavlink_message_t& msg, state::StateStore& store);

}  // namespace protocol
```

- [ ] **Step 4: 写 `src/protocol/extension_decoder.cpp`（本任务只做 NAMED_VALUE_INT 分支）**

```cpp
/**
 * @file extension_decoder.cpp
 * @brief extension_decoder.hpp 的实现。
 */

#include "protocol/extension_decoder.hpp"

#include <array>
#include <cstring>
#include <string_view>

namespace protocol {

namespace {

/// name 字段是 char[10]，不保证有'\0'，用 strnlen 限长取值再比较，避免越界读。
bool DecodeNamedValueInt(const mavlink_named_value_int_t& value, state::StateStore& store) {
  const std::string_view name(value.name, strnlen(value.name, sizeof(value.name)));
  const auto bits = static_cast<std::uint32_t>(value.value);

  if (name == "MODSTAT0") {
    std::array<std::uint8_t, 8> modules{};
    for (std::size_t i = 0; i < modules.size(); ++i) {
      modules[i] = static_cast<std::uint8_t>((bits >> (i * 4)) & 0xF);
    }
    store.UpdateModStatusLow(modules);
    return true;
  }
  if (name == "MODSTAT1") {
    std::array<std::uint8_t, 6> modules{};
    for (std::size_t i = 0; i < modules.size(); ++i) {
      modules[i] = static_cast<std::uint8_t>((bits >> (i * 4)) & 0xF);
    }
    store.UpdateModStatusHigh(modules);
    return true;
  }
  if (name == "BAT2STAT") {
    state::Battery2Status status{};
    status.voltage_mv = static_cast<std::uint16_t>(bits & 0xFFFF);
    status.percent = static_cast<std::uint8_t>((bits >> 16) & 0xFF);
    status.low_voltage = ((bits >> 24) & 0x1) != 0;
    store.UpdateBattery2Status(status);
    return true;
  }
  if (name == "MOTORPWM") {
    state::MotorPwm pwm{};
    for (std::size_t i = 0; i < pwm.duty_percent.size(); ++i) {
      pwm.duty_percent[i] = static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFF);
    }
    store.UpdateMotorPwm(pwm);
    return true;
  }
  if (name == "GNSS_SAT") {
    state::GnssSat sat{};
    sat.gps_visible = static_cast<std::uint8_t>(bits & 0xFF);
    sat.beidou_visible = static_cast<std::uint8_t>((bits >> 8) & 0xFF);
    sat.gps_used = static_cast<std::uint8_t>((bits >> 16) & 0xFF);
    sat.beidou_used = static_cast<std::uint8_t>((bits >> 24) & 0xFF);
    store.UpdateGnssSat(sat);
    return true;
  }
  if (name == "ENVHUM") {
    state::EnvHumidity hum{};
    hum.relative_humidity_x10 = static_cast<std::uint16_t>(bits);
    store.UpdateEnvHumidity(hum);
    return true;
  }
  return false;
}

}  // namespace

bool DecodeExtensionAndStore(const mavlink_message_t& msg, state::StateStore& store) {
  switch (msg.msgid) {
    case MAVLINK_MSG_ID_NAMED_VALUE_INT: {
      mavlink_named_value_int_t decoded{};
      mavlink_msg_named_value_int_decode(&msg, &decoded);
      return DecodeNamedValueInt(decoded, store);
    }
    default:
      return false;
  }
}

}  // namespace protocol
```

- [ ] **Step 5: 运行测试，确认全部通过**

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: 编译零警告；`extension_decoder` 这个 test 通过（8 个 `TEST_CASE` 全绿），其余既有 test（`mavlink_link`/`app_config`/`state_store`/`telemetry_decoder`）不受影响。

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/protocol/extension_decoder.hpp src/protocol/extension_decoder.cpp tests/test_extension_decoder.cpp
git commit -m "$(cat <<'EOF'
feat: 新增NAMED_VALUE_INT扩展帧解码(extension_decoder)
EOF
)"
```

---

### Task 3: `protocol/extension_decoder` —— TUNNEL 字节级解码

**Files:**
- Modify: `src/protocol/extension_decoder.cpp`
- Modify: `tests/test_extension_decoder.cpp`

**Interfaces:**
- Consumes: `state::StateStore`（Task 1）的 `UpdateAlarmTable`/`UpdateMessageLog`。
- Produces: 无新增公开符号——`DecodeExtensionAndStore` 的 `TUNNEL` 分支从 `default: return false;` 变成真正处理 `payload_type=0x8001/0x8002`。

- [ ] **Step 1: 在 `tests/test_extension_decoder.cpp` 追加失败的测试**

在 `namespace { ... }` 匿名命名空间的 `PackNamedValueInt` 之后追加一个 TUNNEL 打包辅助函数：

```cpp
mavlink_message_t PackTunnel(std::uint16_t payload_type, std::uint8_t payload_length,
                              const std::array<std::uint8_t, 128>& payload) {
  mavlink_message_t msg{};
  mavlink_msg_tunnel_pack(kSystemId, kComponentId, &msg, /*target_system=*/0,
                           /*target_component=*/0, payload_type, payload_length, payload.data());
  return msg;
}
```

（这个辅助函数需要 `#include <array>`，加到文件顶部 `#include <cstdint>` 之后。）

然后在文件最后一个 `TEST_CASE`（`"不认识的消息类型被安静忽略"`）之后追加：

```cpp

TEST_CASE("TUNNEL告警表解码出ver/active_count/每行字段") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 1;                          // ver
  payload[1] = 2;                          // active_count
  payload[2] = 3;                          // row0.source_id
  payload[3] = 0x34;
  payload[4] = 0x12;                       // row0.fault_code=0x1234 LE
  payload[5] = 2;                          // row0.severity
  payload[6] = 1;                          // row0.active=true
  payload[7] = 100;
  payload[8] = 0;                          // row0.age_s=100 LE
  payload[9] = 7;                          // row1.source_id
  payload[10] = 0x56;
  payload[11] = 0x00;                      // row1.fault_code=0x0056 LE
  payload[12] = 4;                         // row1.severity
  payload[13] = 0;                         // row1.active=false
  payload[14] = 0x88;
  payload[15] = 0x13;                      // row1.age_s=5000 LE
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x8001, /*payload_length=*/16, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.alarm_table.has_value());
  CHECK(snapshot.alarm_table->ver == 1);
  CHECK(snapshot.alarm_table->active_count == 2);
  CHECK(snapshot.alarm_table->entries[0].source_id == 3);
  CHECK(snapshot.alarm_table->entries[0].fault_code == 0x1234);
  CHECK(snapshot.alarm_table->entries[0].severity == 2);
  CHECK(snapshot.alarm_table->entries[0].active);
  CHECK(snapshot.alarm_table->entries[0].age_s == 100);
  CHECK(snapshot.alarm_table->entries[1].source_id == 7);
  CHECK(snapshot.alarm_table->entries[1].fault_code == 0x0056);
  CHECK(snapshot.alarm_table->entries[1].severity == 4);
  CHECK_FALSE(snapshot.alarm_table->entries[1].active);
  CHECK(snapshot.alarm_table->entries[1].age_s == 5000);
}

TEST_CASE("TUNNEL日志增量解码出latest_seq/count/每条字段") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 42;
  payload[1] = 0;                          // latest_seq=42 LE
  payload[2] = 1;                          // count
  payload[3] = 42;
  payload[4] = 0;                          // entry0.sequence=42 LE
  payload[5] = 7;
  payload[6] = 0;                          // entry0.message_id=7 LE
  payload[7] = 9;
  payload[8] = 30;
  payload[9] = 15;                         // entry0.time_hhmmss={9,30,15}
  payload[10] = 1;                         // entry0.severity=WARNING
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x8002, /*payload_length=*/11, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.message_log.has_value());
  CHECK(snapshot.message_log->latest_seq == 42);
  CHECK(snapshot.message_log->count == 1);
  CHECK(snapshot.message_log->entries[0].sequence == 42);
  CHECK(snapshot.message_log->entries[0].message_id == 7);
  CHECK(snapshot.message_log->entries[0].time_hhmmss[0] == 9);
  CHECK(snapshot.message_log->entries[0].time_hhmmss[1] == 30);
  CHECK(snapshot.message_log->entries[0].time_hhmmss[2] == 15);
  CHECK(snapshot.message_log->entries[0].severity == 1);
}

TEST_CASE("TUNNEL日志增量count=0时只有latest_seq有意义,不是畸形") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 100;
  payload[1] = 0;                          // latest_seq=100 LE
  payload[2] = 0;                          // count=0
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x8002, /*payload_length=*/3, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.message_log.has_value());
  CHECK(snapshot.message_log->latest_seq == 100);
  CHECK(snapshot.message_log->count == 0);
}

TEST_CASE("TUNNEL告警表声明行数超过协议上限14时被clamp到14") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 1;                          // ver
  payload[1] = 200;                        // active_count声明成200(超协议上限)
  payload[2] = 9;                          // row0.source_id(用来验证真正解析出来的行有效)
  payload[3] = 0x01;
  payload[4] = 0x00;                       // row0.fault_code=1
  payload[5] = 0;                          // row0.severity
  payload[6] = 1;                          // row0.active=true
  payload[7] = 1;
  payload[8] = 0;                          // row0.age_s=1
  // payload_length按14行的完整长度给,2+14*7=100,足够容纳,应该clamp到协议硬上限14
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x8001, /*payload_length=*/100, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.alarm_table.has_value());
  CHECK(snapshot.alarm_table->active_count == 14);
  CHECK(snapshot.alarm_table->entries[0].source_id == 9);
}

TEST_CASE("TUNNEL告警表payload_length不够声明行数时被clamp到实际能容纳的行数") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 1;                          // ver
  payload[1] = 10;                         // active_count声明成10(协议上限内)
  payload[2] = 9;                          // row0.source_id
  payload[3] = 1;
  payload[4] = 0;                          // row0.fault_code=1
  payload[5] = 0;                          // row0.severity
  payload[6] = 1;                          // row0.active=true
  payload[7] = 1;
  payload[8] = 0;                          // row0.age_s=1
  // payload_length只够2字节表头+3行(2+3*7=23),声明的10行装不下,应该clamp到3
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x8001, /*payload_length=*/23, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.alarm_table.has_value());
  CHECK(snapshot.alarm_table->active_count == 3);
  CHECK(snapshot.alarm_table->entries[0].source_id == 9);
}

TEST_CASE("TUNNEL payload_length小于表头长度时整帧判畸形,返回false不写入store") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 1;
  mavlink_message_t alarm_msg = PackTunnel(/*payload_type=*/0x8001, /*payload_length=*/1, payload);
  mavlink_message_t log_msg = PackTunnel(/*payload_type=*/0x8002, /*payload_length=*/2, payload);
  state::StateStore store;

  bool alarm_handled = protocol::DecodeExtensionAndStore(alarm_msg, store);
  bool log_handled = protocol::DecodeExtensionAndStore(log_msg, store);

  CHECK_FALSE(alarm_handled);
  CHECK_FALSE(log_handled);
  auto snapshot = store.Snapshot();
  CHECK_FALSE(snapshot.alarm_table.has_value());
  CHECK_FALSE(snapshot.message_log.has_value());
}

TEST_CASE("不认识的payload_type被安静忽略") {
  std::array<std::uint8_t, 128> payload{};
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x9999, /*payload_length=*/10, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK_FALSE(handled);
}
```

- [ ] **Step 2: 运行测试，确认新增用例编译失败/断言失败**

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: `extension_decoder` test 里新增的 7 个 `TUNNEL` 相关 `TEST_CASE` 失败（`handled` 应为 `true` 却拿到 `false`，因为 `TUNNEL` 分支还没实现）。

- [ ] **Step 3: 修改 `src/protocol/extension_decoder.cpp`，实现 TUNNEL 分支**

把顶部 include 从：

```cpp
#include "protocol/extension_decoder.hpp"

#include <array>
#include <cstring>
#include <string_view>
```

改成：

```cpp
#include "protocol/extension_decoder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>
```

在匿名命名空间里，`DecodeNamedValueInt` 函数之前插入常量和字节读取辅助函数、告警表/日志增量解析函数：

```cpp
constexpr std::uint16_t kAlarmTablePayloadType = 0x8001;
constexpr std::size_t kAlarmHeaderSize = 2;
constexpr std::size_t kAlarmRowSize = 7;
constexpr std::size_t kAlarmMaxRows = 14;

constexpr std::uint16_t kMessageLogPayloadType = 0x8002;
constexpr std::size_t kLogHeaderSize = 3;
constexpr std::size_t kLogEntrySize = 8;
constexpr std::size_t kLogMaxEntries = 9;

/// 从裸字节数组按小端读一个 uint16_t，TUNNEL payload 里的多字节字段都是这个格式。
std::uint16_t ReadU16LE(const std::uint8_t* data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset] | (static_cast<std::uint16_t>(data[offset + 1]) << 8));
}

bool DecodeAlarmTable(const mavlink_tunnel_t& value, state::StateStore& store) {
  if (value.payload_length < kAlarmHeaderSize) {
    return false;
  }
  state::AlarmTable table{};
  table.ver = value.payload[0];
  const std::uint8_t declared_count = value.payload[1];
  const std::size_t capacity_rows = (value.payload_length - kAlarmHeaderSize) / kAlarmRowSize;
  table.active_count = std::min({static_cast<std::size_t>(declared_count), kAlarmMaxRows, capacity_rows});

  for (std::size_t i = 0; i < table.active_count; ++i) {
    const std::size_t offset = kAlarmHeaderSize + i * kAlarmRowSize;
    state::AlarmEntry& entry = table.entries[i];
    entry.source_id = value.payload[offset];
    entry.fault_code = ReadU16LE(value.payload, offset + 1);
    entry.severity = value.payload[offset + 3];
    entry.active = value.payload[offset + 4] != 0;
    entry.age_s = ReadU16LE(value.payload, offset + 5);
  }
  store.UpdateAlarmTable(table);
  return true;
}

bool DecodeMessageLog(const mavlink_tunnel_t& value, state::StateStore& store) {
  if (value.payload_length < kLogHeaderSize) {
    return false;
  }
  state::MessageLog log{};
  log.latest_seq = ReadU16LE(value.payload, 0);
  const std::uint8_t declared_count = value.payload[2];
  const std::size_t capacity_entries = (value.payload_length - kLogHeaderSize) / kLogEntrySize;
  log.count = std::min({static_cast<std::size_t>(declared_count), kLogMaxEntries, capacity_entries});

  for (std::size_t i = 0; i < log.count; ++i) {
    const std::size_t offset = kLogHeaderSize + i * kLogEntrySize;
    state::LogEntry& entry = log.entries[i];
    entry.sequence = ReadU16LE(value.payload, offset);
    entry.message_id = ReadU16LE(value.payload, offset + 2);
    entry.time_hhmmss[0] = value.payload[offset + 4];
    entry.time_hhmmss[1] = value.payload[offset + 5];
    entry.time_hhmmss[2] = value.payload[offset + 6];
    entry.severity = value.payload[offset + 7];
  }
  store.UpdateMessageLog(log);
  return true;
}

bool DecodeTunnel(const mavlink_tunnel_t& value, state::StateStore& store) {
  switch (value.payload_type) {
    case kAlarmTablePayloadType:
      return DecodeAlarmTable(value, store);
    case kMessageLogPayloadType:
      return DecodeMessageLog(value, store);
    default:
      return false;
  }
}
```

把 `DecodeExtensionAndStore` 里的 `default: return false;` 分支之前加一个 `case`，从：

```cpp
bool DecodeExtensionAndStore(const mavlink_message_t& msg, state::StateStore& store) {
  switch (msg.msgid) {
    case MAVLINK_MSG_ID_NAMED_VALUE_INT: {
      mavlink_named_value_int_t decoded{};
      mavlink_msg_named_value_int_decode(&msg, &decoded);
      return DecodeNamedValueInt(decoded, store);
    }
    default:
      return false;
  }
}
```

改成：

```cpp
bool DecodeExtensionAndStore(const mavlink_message_t& msg, state::StateStore& store) {
  switch (msg.msgid) {
    case MAVLINK_MSG_ID_NAMED_VALUE_INT: {
      mavlink_named_value_int_t decoded{};
      mavlink_msg_named_value_int_decode(&msg, &decoded);
      return DecodeNamedValueInt(decoded, store);
    }
    case MAVLINK_MSG_ID_TUNNEL: {
      mavlink_tunnel_t decoded{};
      mavlink_msg_tunnel_decode(&msg, &decoded);
      return DecodeTunnel(decoded, store);
    }
    default:
      return false;
  }
}
```

- [ ] **Step 4: 运行测试，确认全部通过**

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: 编译零警告；`extension_decoder` 这个 test 通过（15 个 `TEST_CASE` 全绿：Task 2 的 8 个 + 本任务新增的 7 个），其余既有 test 不受影响。

- [ ] **Step 5: Commit**

```bash
git add src/protocol/extension_decoder.cpp tests/test_extension_decoder.cpp
git commit -m "$(cat <<'EOF'
feat: 新增TUNNEL扩展帧解码(告警表/日志增量)
EOF
)"
```

---

### Task 4: `main.cpp` —— 接入扩展帧解码

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `protocol::DecodeExtensionAndStore`（Task 2/3）、`state::TelemetryState` 新增字段（Task 1）。

这一步是集成，不是新的可单测单元（跟 M3a Task 3 一样：main.cpp 是组合根，靠编译零警告 + 真机人工验证）。

- [ ] **Step 1: 加 include**

把：

```cpp
#include "common/mavlink.h"
#include "config/app_config.hpp"
#include "protocol/telemetry_decoder.hpp"
#include "state/state_store.hpp"
#include "uart/mavlink_link.hpp"
```

改成：

```cpp
#include "common/mavlink.h"
#include "config/app_config.hpp"
#include "protocol/extension_decoder.hpp"
#include "protocol/telemetry_decoder.hpp"
#include "state/state_store.hpp"
#include "uart/mavlink_link.hpp"
```

- [ ] **Step 2: 在匿名命名空间里，`LogTelemetry` 之后新增 `LogExtension`**

```cpp

/// 跟 LogTelemetry 同样的定位：按扩展帧的 msgid/内部语义打印 state_store 里
/// 对应字段的最新值，供真机人工验证；解码逻辑本身在
/// protocol::DecodeExtensionAndStore 里，这里只打印。
void LogExtension(std::uint32_t msgid, const state::TelemetryState& snapshot) {
  switch (msgid) {
    case MAVLINK_MSG_ID_NAMED_VALUE_INT:
      if (snapshot.module_status) {
        std::cout << "MODSTAT: [0]=" << static_cast<int>((*snapshot.module_status)[0])
                  << " [13]=" << static_cast<int>((*snapshot.module_status)[13]) << std::endl;
      }
      if (snapshot.battery2_status) {
        std::cout << "BAT2STAT: voltage_mv=" << snapshot.battery2_status->voltage_mv
                  << " percent=" << static_cast<int>(snapshot.battery2_status->percent)
                  << " low_voltage=" << snapshot.battery2_status->low_voltage << std::endl;
      }
      if (snapshot.motor_pwm) {
        std::cout << "MOTORPWM: [0]=" << static_cast<int>(snapshot.motor_pwm->duty_percent[0])
                  << std::endl;
      }
      if (snapshot.gnss_sat) {
        std::cout << "GNSS_SAT: gps_visible=" << static_cast<int>(snapshot.gnss_sat->gps_visible)
                  << " gps_used=" << static_cast<int>(snapshot.gnss_sat->gps_used) << std::endl;
      }
      if (snapshot.env_humidity) {
        std::cout << "ENVHUM: relative_humidity_x10=" << snapshot.env_humidity->relative_humidity_x10
                  << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_TUNNEL:
      if (snapshot.alarm_table) {
        std::cout << "ALARM_TABLE: active_count=" << snapshot.alarm_table->active_count
                  << std::endl;
      }
      if (snapshot.message_log) {
        std::cout << "MESSAGE_LOG: latest_seq=" << snapshot.message_log->latest_seq
                  << " count=" << snapshot.message_log->count << std::endl;
      }
      break;
    default:
      break;
  }
}
```

- [ ] **Step 3: 改主循环，依次尝试两个解码器**

把：

```cpp
    if (auto msg = link->ReceiveMessage()) {
      if (protocol::DecodeAndStore(*msg, state_store)) {
        LogTelemetry(msg->msgid, state_store.Snapshot());
      }
    }
```

改成：

```cpp
    if (auto msg = link->ReceiveMessage()) {
      if (protocol::DecodeAndStore(*msg, state_store)) {
        LogTelemetry(msg->msgid, state_store.Snapshot());
      } else if (protocol::DecodeExtensionAndStore(*msg, state_store)) {
        LogExtension(msg->msgid, state_store.Snapshot());
      }
    }
```

- [ ] **Step 4: 验证编译零警告，既有测试不受影响**

Run: `cmake -B build -S . && cmake --build build 2>&1 | tee /tmp/build.log && grep -i warning /tmp/build.log && ctest --test-dir build --output-on-failure`
Expected: 构建成功，无警告输出；全部 5 个 test（`mavlink_link`/`app_config`/`state_store`/`telemetry_decoder`/`extension_decoder`）通过。

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
feat: main.cpp接入M3b扩展帧解码
EOF
)"
```

---

### Task 5: 文档同步 —— V1设计文档.md 标记 M3b 已实现

**Files:**
- Modify: `docs/V1设计文档.md`

- [ ] **Step 1: 更新 §10 M3b 里程碑那一条**

把：

```
- **M3b 扩展帧解码**：`NAMED_VALUE_INT`（`MODSTAT0`/`MODSTAT1`/`BAT2STAT`/`MOTORPWM`/`GNSS_SAT`/`ENVHUM`）/`TUNNEL`（告警表/日志增量）语义解析，字段字典见第 4.1/4.2 节
```

改成：

```
- **M3b 扩展帧解码**：`NAMED_VALUE_INT`（`MODSTAT0`/`MODSTAT1`/`BAT2STAT`/`MOTORPWM`/`GNSS_SAT`/`ENVHUM`）/`TUNNEL`（告警表/日志增量）语义解析，字段字典见第 4.1/4.2 节——已实现，见 `docs/superpowers/plans/2026-07-06-m3b-extension-decode.md`
```

- [ ] **Step 2: 检查改动只涉及这一处**

Run: `git diff docs/V1设计文档.md`
Expected: diff 只包含上面这一处替换。

- [ ] **Step 3: Commit**

```bash
git add docs/V1设计文档.md
git commit -m "$(cat <<'EOF'
docs: M3b落地后标记里程碑已实现
EOF
)"
```
