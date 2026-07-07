# M3c 身份帧解码 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 解码 `OPEN_DRONE_ID_*` 系列（BASIC_ID/LOCATION/SYSTEM/OPERATOR_ID/SELF_ID）身份帧、从帧头 `sysid` 解析 DCDW 角色号、读取 RPi 硬件序列号，三者写入 `state_store`，供后续里程碑（M4 JSON 序列化、M5 MQTT topic 寻址）使用。

**Architecture:** OPEN_DRONE_ID_* 是有名字段的官方 struct，解码方式沿用 M3a"存官方 struct 原样"的原则，归属既有的 `protocol::DecodeExtensionAndStore`（`extension_decoder.cpp` 新增 5 个 case）。新增 `protocol/identity.hpp/.cpp` 处理两件跟 MAVLink payload 无关的事（帧头 sysid 格式化、本机序列号读取）外加一个字节数组转字符串的小工具（`ExtractVendorId`，供 BASIC_ID 解码时调用）。`state_store` 新增 8 个字段（5 个原始 struct + vendor_id/dcdw_label/rpi_serial 三个衍生字符串），扁平挂在 `TelemetryState` 上，不引入新的子 struct。

**Tech Stack:** C++23，doctest（现有单测框架），vendor 的官方 `mavlink/c_library_v2` 头文件，CMake。

## Global Constraints

- 官方 MAVLink struct 一律原样存储，不做单位换算（M3a/M3b 既定原则，本计划延续）。
- 涉及"字节数组转字符串"的地方（`uas_id[20]` → `vendor_id`）必须用 `strnlen`，不能假设有 null 终止符——20 字节写满、无终止符是合法输入，不是畸形帧。
- 不对 STM32 上报的任何身份数据做格式/取值校验——RPi 是使用方，不是校验方（`docs/设备标识符.md` §2.3、§4.5）。
- 不实现"身份就绪后再连 MQTT"的启动等待逻辑——那是 M5 的范围，本计划只做解码+存储。
- 生产代码里任何"仅供测试注入"的参数/函数必须在声明处显式注明【测试专用】，并说明真实调用点从不覆盖默认值。
- 每个任务完成后运行对应测试，全绿才能进入下一个任务；每个任务结束提交一次。

---

### Task 1: state_store 新增身份字段

**Files:**
- Modify: `src/state/state_store.hpp`
- Modify: `src/state/state_store.cpp`
- Test: `tests/test_state_store.cpp`

**Interfaces:**
- Consumes: 无（这是最底层的数据结构，不依赖其他任务）。
- Produces：
  - `TelemetryState` 新增 8 个字段：`open_drone_id_basic_id`（`std::optional<mavlink_open_drone_id_basic_id_t>`）、`open_drone_id_location`（`std::optional<mavlink_open_drone_id_location_t>`）、`open_drone_id_system`（`std::optional<mavlink_open_drone_id_system_t>`）、`open_drone_id_operator_id`（`std::optional<mavlink_open_drone_id_operator_id_t>`）、`open_drone_id_self_id`（`std::optional<mavlink_open_drone_id_self_id_t>`）、`vendor_id`（`std::optional<std::string>`）、`dcdw_label`（`std::optional<std::string>`）、`rpi_serial`（`std::optional<std::string>`）。
  - `StateStore` 新增 8 个方法：`UpdateOpenDroneIdBasicId`、`UpdateOpenDroneIdLocation`、`UpdateOpenDroneIdSystem`、`UpdateOpenDroneIdOperatorId`、`UpdateOpenDroneIdSelfId`、`UpdateVendorId`、`UpdateDcdwLabel`、`UpdateRpiSerial`——供 Task 3（extension_decoder）和 Task 4（main.cpp）调用。

- [ ] **Step 1: 写 state_store.hpp 的新字段（先改头文件，不写测试断言前先让类型存在）**

打开 `src/state/state_store.hpp`，在 `#include <optional>` 之后加一行：

```cpp
#include <string>
```

在 `struct TelemetryState { ... };` 的最后一个字段 `std::optional<MessageLog> message_log;` 之后加：

```cpp

  /// OPEN_DRONE_ID_* 身份帧(M3c)，官方 struct 原样存储，不做单位换算/校验。
  std::optional<mavlink_open_drone_id_basic_id_t> open_drone_id_basic_id;
  std::optional<mavlink_open_drone_id_location_t> open_drone_id_location;
  std::optional<mavlink_open_drone_id_system_t> open_drone_id_system;
  std::optional<mavlink_open_drone_id_operator_id_t> open_drone_id_operator_id;
  std::optional<mavlink_open_drone_id_self_id_t> open_drone_id_self_id;

  /// 从 OPEN_DRONE_ID_BASIC_ID.uas_id 提取的厂商唯一产品识别码，RPi 不校验/不重新计算。
  std::optional<std::string> vendor_id;
  /// 从 MAVLink 帧头 sysid 格式化的 DCDW-XXX 角色号，帧头字段，不是 payload 字段。
  std::optional<std::string> dcdw_label;
  /// RPi 本机硬件序列号(/proc/cpuinfo)，V1 过渡期权威键，跟 MAVLink 帧无关。
  std::optional<std::string> rpi_serial;
```

在 `class StateStore` 里，紧跟 `void UpdateMessageLog(const MessageLog& value);` 之后加：

```cpp
  void UpdateOpenDroneIdBasicId(const mavlink_open_drone_id_basic_id_t& value);
  void UpdateOpenDroneIdLocation(const mavlink_open_drone_id_location_t& value);
  void UpdateOpenDroneIdSystem(const mavlink_open_drone_id_system_t& value);
  void UpdateOpenDroneIdOperatorId(const mavlink_open_drone_id_operator_id_t& value);
  void UpdateOpenDroneIdSelfId(const mavlink_open_drone_id_self_id_t& value);
  void UpdateVendorId(const std::string& value);
  void UpdateDcdwLabel(const std::string& value);
  void UpdateRpiSerial(const std::string& value);
```

- [ ] **Step 2: 实现 state_store.cpp 的 8 个新方法**

打开 `src/state/state_store.cpp`，在 `void StateStore::UpdateMessageLog(...) { ... }` 之后、`TelemetryState StateStore::Snapshot() const {` 之前插入：

```cpp
void StateStore::UpdateOpenDroneIdBasicId(const mavlink_open_drone_id_basic_id_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.open_drone_id_basic_id = value;
}

void StateStore::UpdateOpenDroneIdLocation(const mavlink_open_drone_id_location_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.open_drone_id_location = value;
}

void StateStore::UpdateOpenDroneIdSystem(const mavlink_open_drone_id_system_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.open_drone_id_system = value;
}

void StateStore::UpdateOpenDroneIdOperatorId(const mavlink_open_drone_id_operator_id_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.open_drone_id_operator_id = value;
}

void StateStore::UpdateOpenDroneIdSelfId(const mavlink_open_drone_id_self_id_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.open_drone_id_self_id = value;
}

void StateStore::UpdateVendorId(const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.vendor_id = value;
}

void StateStore::UpdateDcdwLabel(const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.dcdw_label = value;
}

void StateStore::UpdateRpiSerial(const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.rpi_serial = value;
}
```

- [ ] **Step 3: 写测试（先写测试代码，再运行验证；这一步不是"先失败后实现"的 TDD 顺序，因为 Step 1/2 已经把实现写好——用测试确认实现正确，等价于验证性测试）**

打开 `tests/test_state_store.cpp`，在文件末尾（最后一个 `TEST_CASE` 之后）加：

```cpp
TEST_CASE("身份类字段各自独立更新,不影响其他字段") {
  state::StateStore store;
  mavlink_open_drone_id_basic_id_t basic_id{};
  basic_id.id_type = 1;
  basic_id.ua_type = 2;
  std::memcpy(basic_id.uas_id, "DCDWCNS1ABCDEFGHIJKL", 20);

  store.UpdateOpenDroneIdBasicId(basic_id);
  store.UpdateVendorId("DCDWCNS1ABCDEFGHIJKL");
  store.UpdateDcdwLabel("DCDW-007");
  store.UpdateRpiSerial("100000001234abcd");

  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.open_drone_id_basic_id.has_value());
  CHECK(snapshot.open_drone_id_basic_id->id_type == 1);
  CHECK(snapshot.open_drone_id_basic_id->ua_type == 2);
  REQUIRE(snapshot.vendor_id.has_value());
  CHECK(*snapshot.vendor_id == "DCDWCNS1ABCDEFGHIJKL");
  REQUIRE(snapshot.dcdw_label.has_value());
  CHECK(*snapshot.dcdw_label == "DCDW-007");
  REQUIRE(snapshot.rpi_serial.has_value());
  CHECK(*snapshot.rpi_serial == "100000001234abcd");
  CHECK_FALSE(snapshot.open_drone_id_location.has_value());
  CHECK_FALSE(snapshot.open_drone_id_system.has_value());
  CHECK_FALSE(snapshot.open_drone_id_operator_id.has_value());
  CHECK_FALSE(snapshot.open_drone_id_self_id.has_value());
}
```

在文件顶部 `#include` 区加一行（如果还没有）：

```cpp
#include <cstring>
```

- [ ] **Step 4: 编译并运行测试确认通过**

Run: `cmake --build build --target test_state_store && ./build/test_state_store`
Expected: 所有 TEST_CASE 通过（包括新加的这一个），退出码 0。

- [ ] **Step 5: Commit**

```bash
git add src/state/state_store.hpp src/state/state_store.cpp tests/test_state_store.cpp
git commit -m "feat: state_store新增M3c身份字段"
```

---

### Task 2: `protocol/identity.hpp/.cpp`

**Files:**
- Create: `src/protocol/identity.hpp`
- Create: `src/protocol/identity.cpp`
- Test: `tests/test_identity.cpp`
- Create（测试 fixture）: `tests/fixtures/cpuinfo_with_serial.txt`
- Create（测试 fixture）: `tests/fixtures/cpuinfo_without_serial.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: 无（纯函数模块，不依赖 state_store）。
- Produces：
  - `std::string protocol::FormatDcdwLabel(std::uint8_t sysid)` —— 供 Task 4（main.cpp）调用。
  - `std::optional<std::string> protocol::ReadRpiSerial(const std::filesystem::path& path = "/proc/cpuinfo")` —— 供 Task 4（main.cpp）调用，真机代码不传第二个参数。
  - `std::string protocol::ExtractVendorId(const std::uint8_t (&uas_id)[20])` —— 供 Task 3（extension_decoder.cpp）解码 `OPEN_DRONE_ID_BASIC_ID` 时调用。

- [ ] **Step 1: 写 identity.hpp**

创建 `src/protocol/identity.hpp`：

```cpp
#pragma once

/**
 * @file identity.hpp
 * @brief M3c 范围内跟 MAVLink 消息 payload 内容无关的身份数据处理：
 * DCDW 角色号格式化、RPi 硬件序列号读取、uas_id 字节数组转字符串。
 *
 * @details
 * `OPEN_DRONE_ID_*` 消息本身的解码在 extension_decoder.hpp/.cpp 里(跟
 * NAMED_VALUE_INT/TUNNEL 同一个文件，同一个 DecodeExtensionAndStore 函数)。
 * 这个文件只处理三件更底层的事：帧头 sysid 格式化(不是 payload 字段)、
 * 本机文件读取(跟 MAVLink 帧完全无关)、uas_id 字节数组转字符串(供
 * extension_decoder.cpp 调用，因为提取逻辑跟"身份"这个概念强相关)。
 * 依赖边界：只依赖标准库，不包含 state/、uart/ 等模块头文件。
 */

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace protocol {

/**
 * @brief 从 MAVLink 帧头 sysid 格式化 DCDW 角色号，3 位数字补零。
 * @param sysid MAVLink 帧头的 system_id 字段(uint8_t，最大 255，3 位数字够用)。
 * @return 形如 "DCDW-007" 的字符串。
 * @details 依据 docs/设备标识符.md §3：这个数字就是固件的 PX4LITE_UNIT_ID，
 * 也是 MAVLink 帧头的 system_id，不是某个消息 payload 里的字段。
 */
std::string FormatDcdwLabel(std::uint8_t sysid);

/**
 * @brief 读取 RPi 本机硬件序列号(/proc/cpuinfo 的 Serial 行)。
 * @param path 【测试专用】仅供单元测试注入 fixture 文件(tests/test_identity.cpp)，
 * 真机代码(main.cpp)从不显式传参，永远用默认值读真实 /proc/cpuinfo。
 * M3c 真机验证通过后，评估是否删掉这个参数(连同依赖它的 fixture 测试一起去掉)
 * 或注释掉，不作为长期对外接口保留。
 * @return 找到 Serial 行则返回其值(去掉前后空白)；文件不存在或没有 Serial 行
 * 则返回 std::nullopt——V1 过渡期字段，读不到不是错误，只是没有这个信息。
 */
std::optional<std::string> ReadRpiSerial(const std::filesystem::path& path = "/proc/cpuinfo");

/**
 * @brief 从 uas_id(20 字节，未用部分填 null)提取厂商唯一产品识别码字符串。
 * @param uas_id 对应 mavlink_open_drone_id_basic_id_t::uas_id 的原始字段
 * (uint8_t[20]，C 数组，按引用传递保留长度信息，调用点直接传 value.uas_id)。
 * @return 用 strnlen 求实际长度后转成的字符串，不做格式校验(RPi 不校验身份数据，
 * 见 docs/设备标识符.md §2.3)。20 字节写满、无 null 终止符是合法输入，
 * 此时返回整 20 字节转成的字符串，不是错误。
 */
std::string ExtractVendorId(const std::uint8_t (&uas_id)[20]);

}  // namespace protocol
```

- [ ] **Step 2: 写 identity.cpp**

创建 `src/protocol/identity.cpp`：

```cpp
/**
 * @file identity.cpp
 * @brief identity.hpp 的实现。
 */

#include "protocol/identity.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace protocol {

std::string FormatDcdwLabel(std::uint8_t sysid) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "DCDW-%03u", static_cast<unsigned>(sysid));
  return std::string(buf);
}

std::optional<std::string> ReadRpiSerial(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::nullopt;
  }
  std::string line;
  while (std::getline(file, line)) {
    constexpr std::string_view kPrefix = "Serial";
    if (line.compare(0, kPrefix.size(), kPrefix) != 0) {
      continue;
    }
    const auto colon_pos = line.find(':');
    if (colon_pos == std::string::npos) {
      continue;
    }
    std::string value = line.substr(colon_pos + 1);
    const auto first = value.find_first_not_of(" \t");
    const auto last = value.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
      return std::nullopt;
    }
    return value.substr(first, last - first + 1);
  }
  return std::nullopt;
}

std::string ExtractVendorId(const std::uint8_t (&uas_id)[20]) {
  const char* data = reinterpret_cast<const char*>(uas_id);
  return std::string(data, strnlen(data, 20));
}

}  // namespace protocol
```

需要在文件顶部加 `#include <string_view>`（`kPrefix` 用到）：

```cpp
#include <string_view>
```

- [ ] **Step 3: 加测试 fixture 文件**

创建 `tests/fixtures/cpuinfo_with_serial.txt`（内容模拟真实 `/proc/cpuinfo` 尾部）：

```
Hardware	: BCM2835
Revision	: c03130
Serial		: 100000001234abcd
Model		: Raspberry Pi 5 Model B Rev 1.0
```

创建 `tests/fixtures/cpuinfo_without_serial.txt`（没有 Serial 行）：

```
Hardware	: BCM2835
Revision	: c03130
Model		: Raspberry Pi 5 Model B Rev 1.0
```

- [ ] **Step 4: 写 test_identity.cpp**

创建 `tests/test_identity.cpp`：

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "protocol/identity.hpp"

TEST_CASE("FormatDcdwLabel对sysid=0补零到DCDW-000") {
  CHECK(protocol::FormatDcdwLabel(0) == "DCDW-000");
}

TEST_CASE("FormatDcdwLabel对sysid=1补零到DCDW-001") {
  CHECK(protocol::FormatDcdwLabel(1) == "DCDW-001");
}

TEST_CASE("FormatDcdwLabel对sysid=255(uint8_t最大值)输出DCDW-255") {
  CHECK(protocol::FormatDcdwLabel(255) == "DCDW-255");
}

TEST_CASE("ReadRpiSerial从fixture文件正确解析Serial行") {
  auto serial = protocol::ReadRpiSerial("tests/fixtures/cpuinfo_with_serial.txt");
  REQUIRE(serial.has_value());
  CHECK(*serial == "100000001234abcd");
}

TEST_CASE("ReadRpiSerial在没有Serial行的文件里返回nullopt") {
  auto serial = protocol::ReadRpiSerial("tests/fixtures/cpuinfo_without_serial.txt");
  CHECK_FALSE(serial.has_value());
}

TEST_CASE("ReadRpiSerial在文件不存在时返回nullopt") {
  auto serial = protocol::ReadRpiSerial("tests/fixtures/cpuinfo_does_not_exist.txt");
  CHECK_FALSE(serial.has_value());
}

TEST_CASE("ExtractVendorId从uas_id中间有null的情况正确提取") {
  std::uint8_t uas_id[20] = {'D', 'C', 'D', 'W', 'C', 'N', 'S', '1', 'A', 'B',
                              'C', 'D', 'E', 'F', 0, 0, 0, 0, 0, 0};
  CHECK(protocol::ExtractVendorId(uas_id) == "DCDWCNS1ABCDEF");
}

TEST_CASE("ExtractVendorId在uas_id写满20字节无null终止符时提取整20字节") {
  std::uint8_t uas_id[20] = {'D', 'C', 'D', 'W', 'C', 'N', 'S', '1', 'A', 'B',
                              'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K', 'L', 'M'};
  auto result = protocol::ExtractVendorId(uas_id);
  CHECK(result.size() == 20);
  CHECK(result == "DCDWCNS1ABCDEFGHJKLM");
}
```

- [ ] **Step 5: 把 identity.cpp 加进 CMakeLists.txt 的库源文件，并加测试可执行文件**

打开 `CMakeLists.txt`，在 `add_library(cns_rpi_core ...)` 的源文件列表里，`src/protocol/extension_decoder.cpp` 之后加一行：

```cmake
    src/protocol/identity.cpp
```

在 `add_executable(test_extension_decoder ...)` 那三行之后加：

```cmake

add_executable(test_identity tests/test_identity.cpp)
target_link_libraries(test_identity PRIVATE cns_rpi_core)
add_test(NAME identity COMMAND test_identity)
```

- [ ] **Step 6: 配置、编译、运行测试**

Run: `cmake -B build -S . && cmake --build build --target test_identity`
Expected: 编译成功，无警告无错误。

Run: `cd build && ctest -R identity --output-on-failure && cd ..`
Expected: 所有 TEST_CASE 通过。

**注意**：`ReadRpiSerial` 的 fixture 测试用的是相对路径 `tests/fixtures/...`，测试可执行文件默认工作目录是 `build/`（CMake/ctest 的默认行为），如果测试跑起来报"文件不存在"（`ReadRpiSerial从fixture文件正确解析Serial行` 这条应该 PASS 但如果失败了返回的是 nullopt 而不是 REQUIRE 失败在别的地方），检查是不是工作目录问题——用 `ctest --test-dir build -R identity --output-on-failure` 从仓库根目录跑（ctest 默认从它自己的 `--test-dir` 指定目录跑测试可执行文件，工作目录就是仓库根目录下的相对路径能正确解析到 `tests/fixtures/`）。

- [ ] **Step 7: Commit**

```bash
git add src/protocol/identity.hpp src/protocol/identity.cpp tests/test_identity.cpp tests/fixtures/cpuinfo_with_serial.txt tests/fixtures/cpuinfo_without_serial.txt CMakeLists.txt
git commit -m "feat: 新增protocol/identity(DCDW角色号/RPi序列号/vendor_id提取)"
```

---

### Task 3: extension_decoder 接入 OPEN_DRONE_ID_*

**Files:**
- Modify: `src/protocol/extension_decoder.hpp`
- Modify: `src/protocol/extension_decoder.cpp`
- Test: `tests/test_extension_decoder.cpp`

**Interfaces:**
- Consumes:
  - `state::StateStore::UpdateOpenDroneIdBasicId/Location/System/OperatorId/SelfId/UpdateVendorId`（Task 1）。
  - `protocol::ExtractVendorId(const std::uint8_t (&uas_id)[20])`（Task 2）。
- Produces: `protocol::DecodeExtensionAndStore` 现在也认识 5 个 `OPEN_DRONE_ID_*` msgid（供 Task 4 main.cpp 的 `LogExtension` 读取 snapshot 里对应字段）。

- [ ] **Step 1: 更新 extension_decoder.hpp 的文档注释（说明范围已扩大到 M3c）**

打开 `src/protocol/extension_decoder.hpp`，把文件头的 `@details` 注释里这一句：

```
 * 只负责"认出 M3b 关心的扩展帧语义(MODSTAT0/MODSTAT1/BAT2STAT/MOTORPWM/
 * GNSS_SAT/ENVHUM 六种 NAMED_VALUE_INT + 告警表/日志增量两种 TUNNEL)、拆包、
 * 写入 state_store"，不做单位换算(留给 M4 payload/json_serializer)，
```

改成：

```
 * 只负责"认出本模块关心的扩展帧语义(M3b: MODSTAT0/MODSTAT1/BAT2STAT/MOTORPWM/
 * GNSS_SAT/ENVHUM 六种 NAMED_VALUE_INT + 告警表/日志增量两种 TUNNEL；
 * M3c: OPEN_DRONE_ID_BASIC_ID/LOCATION/SYSTEM/OPERATOR_ID/SELF_ID 五种身份帧)、
 * 解码、写入 state_store"，不做单位换算(留给 M4 payload/json_serializer)，
```

- [ ] **Step 2: 在 extension_decoder.cpp 里加 OPEN_DRONE_ID_BASIC_ID 解码分支**

打开 `src/protocol/extension_decoder.cpp`，在文件顶部 `#include "protocol/extension_decoder.hpp"` 之后加：

```cpp
#include "protocol/identity.hpp"
```

在匿名命名空间 `namespace { ... }` 内、`DecodeTunnel` 函数之后加一个新函数 `DecodeBasicId`：

```cpp
bool DecodeBasicId(const mavlink_open_drone_id_basic_id_t& value, state::StateStore& store) {
  store.UpdateOpenDroneIdBasicId(value);
  store.UpdateVendorId(ExtractVendorId(value.uas_id));
  return true;
}
```

- [ ] **Step 3: 在 `DecodeExtensionAndStore` 的 switch 里加 5 个 case**

在 `bool DecodeExtensionAndStore(...)` 函数的 `switch (msg.msgid) {` 里，`case MAVLINK_MSG_ID_TUNNEL: { ... }` 之后、`default: return false;` 之前加：

```cpp
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID: {
      mavlink_open_drone_id_basic_id_t decoded{};
      mavlink_msg_open_drone_id_basic_id_decode(&msg, &decoded);
      return DecodeBasicId(decoded, store);
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_LOCATION: {
      mavlink_open_drone_id_location_t decoded{};
      mavlink_msg_open_drone_id_location_decode(&msg, &decoded);
      store.UpdateOpenDroneIdLocation(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SYSTEM: {
      mavlink_open_drone_id_system_t decoded{};
      mavlink_msg_open_drone_id_system_decode(&msg, &decoded);
      store.UpdateOpenDroneIdSystem(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_OPERATOR_ID: {
      mavlink_open_drone_id_operator_id_t decoded{};
      mavlink_msg_open_drone_id_operator_id_decode(&msg, &decoded);
      store.UpdateOpenDroneIdOperatorId(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SELF_ID: {
      mavlink_open_drone_id_self_id_t decoded{};
      mavlink_msg_open_drone_id_self_id_decode(&msg, &decoded);
      store.UpdateOpenDroneIdSelfId(decoded);
      return true;
    }
```

- [ ] **Step 4: 加测试——5 个消息的解码 + vendor_id 提取的两种情况**

打开 `tests/test_extension_decoder.cpp`，在文件顶部 `#include <array>` 之后加两个新用例要用到的头文件：

```cpp
#include <cstring>
#include <string_view>
```

在匿名命名空间的 `PackTunnel` 函数之后加 5 个打包辅助函数：

```cpp
mavlink_message_t PackBasicId(std::uint8_t id_type, std::uint8_t ua_type,
                                const std::uint8_t (&uas_id)[20]) {
  mavlink_message_t msg{};
  std::uint8_t id_or_mac[20] = {};
  mavlink_msg_open_drone_id_basic_id_pack(kSystemId, kComponentId, &msg,
                                            /*target_system=*/0, /*target_component=*/0,
                                            id_or_mac, id_type, ua_type, uas_id);
  return msg;
}

mavlink_message_t PackLocation() {
  mavlink_message_t msg{};
  std::uint8_t id_or_mac[20] = {};
  mavlink_msg_open_drone_id_location_pack(
      kSystemId, kComponentId, &msg, /*target_system=*/0, /*target_component=*/0, id_or_mac,
      /*status=*/1, /*direction=*/100, /*speed_horizontal=*/200, /*speed_vertical=*/-50,
      /*latitude=*/313000000, /*longitude=*/1213000000, /*altitude_barometric=*/10.5F,
      /*altitude_geodetic=*/11.5F, /*height_reference=*/0, /*height=*/5.0F,
      /*horizontal_accuracy=*/1, /*vertical_accuracy=*/1, /*barometer_accuracy=*/1,
      /*speed_accuracy=*/1, /*timestamp=*/123.0F, /*timestamp_accuracy=*/1);
  return msg;
}

mavlink_message_t PackSystem() {
  mavlink_message_t msg{};
  std::uint8_t id_or_mac[20] = {};
  mavlink_msg_open_drone_id_system_pack(
      kSystemId, kComponentId, &msg, /*target_system=*/0, /*target_component=*/0, id_or_mac,
      /*operator_location_type=*/0, /*classification_type=*/0, /*operator_latitude=*/313000000,
      /*operator_longitude=*/1213000000, /*area_count=*/1, /*area_radius=*/0,
      /*area_ceiling=*/-1000.0F, /*area_floor=*/-1000.0F, /*category_eu=*/0, /*class_eu=*/0,
      /*operator_altitude_geo=*/-1000.0F, /*timestamp=*/1700000000U);
  return msg;
}

mavlink_message_t PackOperatorId(const char* operator_id) {
  mavlink_message_t msg{};
  std::uint8_t id_or_mac[20] = {};
  mavlink_msg_open_drone_id_operator_id_pack(kSystemId, kComponentId, &msg,
                                               /*target_system=*/0, /*target_component=*/0,
                                               id_or_mac, /*operator_id_type=*/0, operator_id);
  return msg;
}

mavlink_message_t PackSelfId(const char* description) {
  mavlink_message_t msg{};
  std::uint8_t id_or_mac[20] = {};
  mavlink_msg_open_drone_id_self_id_pack(kSystemId, kComponentId, &msg, /*target_system=*/0,
                                           /*target_component=*/0, id_or_mac,
                                           /*description_type=*/0, description);
  return msg;
}
```

在文件末尾加测试用例：

```cpp
TEST_CASE("OPEN_DRONE_ID_BASIC_ID解码存储原始struct并提取vendor_id") {
  std::uint8_t uas_id[20] = {'D', 'C', 'D', 'W', 'C', 'N', 'S', '1', 'A', 'B',
                              'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K', 'L', 'M'};
  mavlink_message_t msg = PackBasicId(/*id_type=*/1, /*ua_type=*/2, uas_id);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.open_drone_id_basic_id.has_value());
  CHECK(snapshot.open_drone_id_basic_id->id_type == 1);
  CHECK(snapshot.open_drone_id_basic_id->ua_type == 2);
  REQUIRE(snapshot.vendor_id.has_value());
  CHECK(*snapshot.vendor_id == "DCDWCNS1ABCDEFGHJKLM");
}

TEST_CASE("OPEN_DRONE_ID_BASIC_ID的uas_id中间有null时vendor_id按strnlen截断") {
  std::uint8_t uas_id[20] = {'D', 'C', 'D', 'W', 'C', 'N', 'S', '1', 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  mavlink_message_t msg = PackBasicId(/*id_type=*/1, /*ua_type=*/0, uas_id);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.vendor_id.has_value());
  CHECK(*snapshot.vendor_id == "DCDWCNS1");
}

TEST_CASE("OPEN_DRONE_ID_LOCATION解码存储原始struct") {
  mavlink_message_t msg = PackLocation();
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.open_drone_id_location.has_value());
  CHECK(snapshot.open_drone_id_location->latitude == 313000000);
  CHECK(snapshot.open_drone_id_location->longitude == 1213000000);
  CHECK(snapshot.open_drone_id_location->speed_vertical == -50);
}

TEST_CASE("OPEN_DRONE_ID_SYSTEM解码存储原始struct") {
  mavlink_message_t msg = PackSystem();
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.open_drone_id_system.has_value());
  CHECK(snapshot.open_drone_id_system->operator_latitude == 313000000);
  CHECK(snapshot.open_drone_id_system->timestamp == 1700000000U);
}

TEST_CASE("OPEN_DRONE_ID_OPERATOR_ID解码存储原始struct") {
  mavlink_message_t msg = PackOperatorId("CAA123456");
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.open_drone_id_operator_id.has_value());
  CHECK(std::string_view(snapshot.open_drone_id_operator_id->operator_id,
                          strnlen(snapshot.open_drone_id_operator_id->operator_id, 20)) ==
        "CAA123456");
}

TEST_CASE("OPEN_DRONE_ID_SELF_ID解码存储原始struct") {
  mavlink_message_t msg = PackSelfId("training kit demo");
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.open_drone_id_self_id.has_value());
  CHECK(std::string_view(snapshot.open_drone_id_self_id->description,
                          strnlen(snapshot.open_drone_id_self_id->description, 23)) ==
        "training kit demo");
}
```

- [ ] **Step 5: 编译运行测试**

Run: `cmake --build build --target test_extension_decoder && ./build/test_extension_decoder`
Expected: 所有 TEST_CASE 通过（含既有的 M3b 用例和新加的 7 个 M3c 用例），无警告无错误。

- [ ] **Step 6: Commit**

```bash
git add src/protocol/extension_decoder.hpp src/protocol/extension_decoder.cpp tests/test_extension_decoder.cpp
git commit -m "feat: extension_decoder接入OPEN_DRONE_ID_*身份帧解码"
```

---

### Task 4: main.cpp 接入

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes:
  - `protocol::ReadRpiSerial()`、`protocol::FormatDcdwLabel(std::uint8_t)`（Task 2）。
  - `state::StateStore::UpdateRpiSerial/UpdateDcdwLabel`（Task 1）。
  - `state::TelemetryState` 的 5 个 `open_drone_id_*` 字段 + `vendor_id`/`dcdw_label`/`rpi_serial`（Task 1）。
  - `protocol::DecodeExtensionAndStore` 现在也认识 5 个新 msgid（Task 3）。
- Produces: 无（这是集成层的最后一站，main.cpp 不被其他任务消费）。

- [ ] **Step 1: 加 include**

打开 `src/main.cpp`，在 `#include "protocol/extension_decoder.hpp"` 之后加：

```cpp
#include "protocol/identity.hpp"
```

- [ ] **Step 2: `LogExtension` 加 5 个 OPEN_DRONE_ID_* 分支 + vendor_id/dcdw_label 打印**

在 `void LogExtension(...)` 函数里，`case MAVLINK_MSG_ID_TUNNEL: { ... }` 的 `break;` 之后、`default: break;` 之前加：

```cpp
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID:
      if (snapshot.open_drone_id_basic_id) {
        std::cout << "OPEN_DRONE_ID_BASIC_ID: id_type="
                  << static_cast<int>(snapshot.open_drone_id_basic_id->id_type)
                  << " ua_type=" << static_cast<int>(snapshot.open_drone_id_basic_id->ua_type)
                  << std::endl;
      }
      if (snapshot.vendor_id) {
        std::cout << "vendor_id=" << *snapshot.vendor_id << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_LOCATION:
      if (snapshot.open_drone_id_location) {
        std::cout << "OPEN_DRONE_ID_LOCATION: lat=" << snapshot.open_drone_id_location->latitude
                  << " lon=" << snapshot.open_drone_id_location->longitude << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SYSTEM:
      if (snapshot.open_drone_id_system) {
        std::cout << "OPEN_DRONE_ID_SYSTEM: operator_lat="
                  << snapshot.open_drone_id_system->operator_latitude << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_OPERATOR_ID:
      if (snapshot.open_drone_id_operator_id) {
        std::cout << "OPEN_DRONE_ID_OPERATOR_ID: operator_id_type="
                  << static_cast<int>(snapshot.open_drone_id_operator_id->operator_id_type)
                  << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SELF_ID:
      if (snapshot.open_drone_id_self_id) {
        std::cout << "OPEN_DRONE_ID_SELF_ID: description_type="
                  << static_cast<int>(snapshot.open_drone_id_self_id->description_type)
                  << std::endl;
      }
      break;
```

把 `LogExtension` 函数前的说明注释：

```
/// 跟 LogTelemetry 同样的定位：按扩展帧的 msgid/内部语义打印 state_store 里
/// 对应字段的最新值，供真机人工验证；解码逻辑本身在
/// protocol::DecodeExtensionAndStore 里，这里只打印。
```

改成：

```
/// 跟 LogTelemetry 同样的定位：按扩展帧(M3b)/身份帧(M3c)的 msgid/内部语义
/// 打印 state_store 里对应字段的最新值，供真机人工验证；解码逻辑本身在
/// protocol::DecodeExtensionAndStore 里，这里只打印。
```

- [ ] **Step 3: main() 里加 dcdw_label 更新（每条收到的帧）+ 启动时读 rpi_serial**

打开 `src/main.cpp`，在 `state::StateStore state_store;` 之后加：

```cpp
  if (auto serial = protocol::ReadRpiSerial()) {
    state_store.UpdateRpiSerial(*serial);
  }
```

在 `while (true) { if (auto msg = link->ReceiveMessage()) {` 内部，`if (protocol::DecodeAndStore(*msg, state_store)) {` 这一行之前加：

```cpp
      state_store.UpdateDcdwLabel(protocol::FormatDcdwLabel(msg->sysid));
```

（即：不管后面 decode 成不成功，只要收到帧就先更新 dcdw_label，因为 sysid 在每条合法帧头里都有。）

- [ ] **Step 4: 更新文件头注释和启动横幅到 M3c**

把文件头 `@details` 注释：

```
 * M3a 阶段在 M2 双向收发闭环基础上接入遥测解码，M3b 阶段接入扩展帧解码：
 * 收到帧依次尝试 protocol::DecodeAndStore（标准遥测）和
 * protocol::DecodeExtensionAndStore（NAMED_VALUE_INT/TUNNEL扩展帧），
 * 写入 state::StateStore -> 打印解码后的有意义字段做人工验证。
 * 不接 MQTT（M5 的事），不处理身份帧（M3c 的事）。
```

改成：

```
 * M3a 阶段在 M2 双向收发闭环基础上接入遥测解码，M3b 阶段接入扩展帧解码，
 * M3c 阶段接入身份帧解码：收到帧先更新 DCDW 角色号(帧头 sysid)，再依次尝试
 * protocol::DecodeAndStore（标准遥测）和 protocol::DecodeExtensionAndStore
 * （NAMED_VALUE_INT/TUNNEL 扩展帧 + OPEN_DRONE_ID_* 身份帧），
 * 写入 state::StateStore -> 打印解码后的有意义字段做人工验证。
 * 启动时读一次 RPi 本机序列号(V1 过渡期权威键)。不接 MQTT（M5 的事）。
```

把 `std::cout << "cns_rpi M3b 启动，串口=" ...` 改成：

```cpp
  std::cout << "cns_rpi M3c 启动，串口=" << app_config->serial.device
            << " 波特率=" << app_config->serial.baud << std::endl;
```

- [ ] **Step 5: 编译并本地运行确认没有回归**

Run: `cmake --build build --target cns_rpi`
Expected: 编译成功，无警告无错误。

Run: `ctest --test-dir build --output-on-failure`
Expected: 全部测试通过（`mavlink_link`/`app_config`/`state_store`/`telemetry_decoder`/`extension_decoder`/`identity` 六个测试套件全绿）。

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "feat: main.cpp接入M3c身份帧解码"
```

---

### Task 5: 文档同步

**Files:**
- Modify: `docs/V1设计文档.md`

**Interfaces:**
- Consumes: 无。
- Produces: 无（文档任务，不产出代码接口）。

- [ ] **Step 1: 标记 M3c 已实现**

打开 `docs/V1设计文档.md`，找到第 10 节这一行：

```
- **M3c 身份帧解码**：OPEN_DRONE_ID_* 系列 + DCDW角色号解析 + RPi序列号打标
```

改成：

```
- **M3c 身份帧解码**：OPEN_DRONE_ID_* 系列 + DCDW角色号解析 + RPi序列号打标——已实现，见 `docs/superpowers/plans/2026-07-07-m3c-identity-decode.md`
```

- [ ] **Step 2: 检查第 9/11 节里"待 M3c 完成"的表述是否需要更新**

打开 `docs/V1设计文档.md`，搜索"待 M3c"和"等 M3c"，逐处确认：

- 第 9 节"Topic 里的设备寻址字段..."那句提到"topic 命名方案的最终定稿要等 M3c...完成之后才能真正拍板"——**这句本身不需要改**，因为"能拿到厂商唯一产品识别码"这件事在 M3c 实现后确实成立了，但"topic 命名方案定稿"是 M5 的事，M3c 完成只是解除了这个依赖，不代表 topic 方案本身现在就定了。保持原文不动。
- 第 11 节"Topic 命名方案最终字符串格式——待 M3c...完成、能拿到厂商唯一产品识别码后定稿"——同上，保持不动，这是指向 M5 阶段要做的事，不是 M3c 自己要做的事。

这一步预期**不修改任何文字**，只是确认没有遗漏需要同步的地方——如果确认后发现有需要改的地方，照实改。

- [ ] **Step 3: Commit**

```bash
git add docs/V1设计文档.md
git commit -m "docs: M3c落地后标记里程碑已实现"
```

---

## 完成后（不属于本计划任务，供执行者知悉）

- `docs/固件对接-数据格式.md` 之前已经写好但**暂不提交**（用户要求等 M3c 实现完再提交）——本计划 5 个任务全部完成、真机验证通过后，记得连同这份文档一起提交。
- 真机验证：按 M2/M3a/M3b 的既定模式，5 个任务全部完成后同步到 `dcdw@192.168.11.4`，`rm -rf build && cmake -B build -S . && cmake --build build`，`ctest --test-dir build --output-on-failure`，确认零警告零错误、全部测试通过。
