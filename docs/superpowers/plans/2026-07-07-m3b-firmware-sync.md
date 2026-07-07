# M3b 字段字典跟固件同步 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 RPi 侧 `NAMED_VALUE_INT` 解码跟固件 `Formal_Framework` PR#1 已经落地的实际 wire 格式同步：`ENVHUM`→`HUMIDITY` 改名、单条 `MOTORPWM`→`MOTOR12`/`MOTOR34` 双帧换分支、新增 `LORASTAT`/`RIDSTAT` 两条 RPi 专属遥测解码。

**Architecture:** 延续 M3b 已有的模式，改动集中在 `protocol::DecodeNamedValueInt`（`extension_decoder.cpp`）一个函数里增删 `if (name == ...)` 分支，`state_store` 相应增删字段/方法，不引入新文件、不改变整体架构。

**Tech Stack:** C++23，doctest（现有单测框架），vendor 的官方 `mavlink/c_library_v2` 头文件，CMake。

## Global Constraints

- 固件不再发送的旧 name（`ENVHUM`/`MOTORPWM`）对应的解码分支直接删除，不保留、不注释掉。
- 官方/固件发的数据原样存储，不做单位换算、不做跨字段一致性校验（`LoraStatus.link_state` 和 `module_status[4]` 冗余但不互相校验，两条帧各自独立更新）。
- `run_state`/`speed_level` 类"两帧冗余拷贝"的字段直接覆盖式更新；`duty_percent` 类"两帧分别负责一半"的字段需要 lazy 零初始化+各写各的一半（沿用 `UpdateModStatusLow`/`High` 模式）。
- `LoraStatus`/`RemoteIdStatus` 各自是独立的具名 struct + `TelemetryState` 上各挂一个 `std::optional`，不合并进 `module_status`，也不用通用容器/variant——跟 `GnssSat`/`Battery2Status` 同一个模式。
- 每个任务完成后运行对应测试，全绿才能进入下一个任务；每个任务结束提交一次。

---

### Task 1: state_store 结构调整

**Files:**
- Modify: `src/state/state_store.hpp`
- Modify: `src/state/state_store.cpp`
- Test: `tests/test_state_store.cpp`

**Interfaces:**
- Consumes: 无（最底层数据结构改动）。
- Produces：
  - `state::MotorPwm` 新增字段 `run_state`(bool)、`speed_level`(uint8_t)。
  - 新增 `state::LoraStatus{loss_rate_x10, node_id, present, link_state}`、`state::RemoteIdStatus{location_count, error_count, last_success_ms}`。
  - `TelemetryState` 新增 `lora_status`/`remote_id_status` 两个 optional 字段。
  - `StateStore` 新增 `UpdateMotorPwmLow(duty0, duty1, run_state, speed_level)`、`UpdateMotorPwmHigh(duty2, duty3, run_state, speed_level)`、`UpdateLoraStatus`、`UpdateRemoteIdStatus`——供 Task 2（`extension_decoder.cpp`）调用。**本任务暂不删除 `UpdateMotorPwm`**：`src/protocol/extension_decoder.cpp` 现在还在调用它（`MOTORPWM` 分支），要等 Task 2 把那个分支删掉之后，`UpdateMotorPwm` 才没有调用方，届时在 Task 2 里一并删除——否则本任务改完后整个仓库编译不过，违反"每个任务都要能独立编译通过"的要求。

- [ ] **Step 1: 修改 `state_store.hpp` 的 struct 定义**

打开 `src/state/state_store.hpp`，把：

```cpp
/// MOTORPWM 拆包结果：最多 4 个电机的占空比(%)。
struct MotorPwm {
  std::array<std::uint8_t, 4> duty_percent;
};
```

改成：

```cpp
/// MOTOR12/MOTOR34 拆包结果：4 个电机的占空比(%)，外加两帧共同携带的
/// run_state/speed_level(整机状态的冗余拷贝，两帧值相同，直接覆盖式更新，
/// 不像 duty_percent 那样需要区分"自己负责哪一半")。
struct MotorPwm {
  std::array<std::uint8_t, 4> duty_percent;
  bool run_state;
  std::uint8_t speed_level;
};

/// LORASTAT 拆包结果：LoRa 链路状态(RPi 专属，只发 USART1，不上 LoRa)。
/// link_state 跟 module_status[4](LORA 模块的粗粒度状态)语义重复，但两条帧
/// 来自不同的固件消息、独立更新，不做一致性校验——state_store 存固件发的
/// 数据原样，不做二次加工。
struct LoraStatus {
  std::uint16_t loss_rate_x10;
  std::uint8_t node_id;
  bool present;
  std::uint8_t link_state;
};

/// RIDSTAT 拆包结果：RemoteID 广播状态(RPi 专属，只发 USART1)。
/// location_count/error_count 是增量语义的计数器低16位，不是绝对值。
struct RemoteIdStatus {
  std::uint16_t location_count;
  std::uint16_t error_count;
  std::uint32_t last_success_ms;
};
```

在 `TelemetryState` 里，把：

```cpp
  std::optional<EnvHumidity> env_humidity;
  std::optional<AlarmTable> alarm_table;
```

改成：

```cpp
  std::optional<EnvHumidity> env_humidity;
  std::optional<LoraStatus> lora_status;
  std::optional<RemoteIdStatus> remote_id_status;
  std::optional<AlarmTable> alarm_table;
```

在 `class StateStore` 里，紧跟着已有的：

```cpp
  void UpdateMotorPwm(const MotorPwm& value);
```

这一行**保留不动**（Task 2 才会删除它），在它之后加：

```cpp
  /// 只写 duty_percent 的 0-1 号索引(来自 MOTOR12)。若 motor_pwm 之前还没有值
  /// (两帧都还没收到过)，先把整个 struct 零初始化；run_state/speed_level 是
  /// 两帧共同的冗余拷贝，每次都直接覆盖(不需要判断"谁负责哪部分")。
  void UpdateMotorPwmLow(std::uint8_t duty0, std::uint8_t duty1, bool run_state,
                          std::uint8_t speed_level);
  /// 只写 duty_percent 的 2-3 号索引(来自 MOTOR34)，语义同上。
  void UpdateMotorPwmHigh(std::uint8_t duty2, std::uint8_t duty3, bool run_state,
                           std::uint8_t speed_level);
```

紧跟 `void UpdateEnvHumidity(const EnvHumidity& value);` 之后加：

```cpp
  void UpdateLoraStatus(const LoraStatus& value);
  void UpdateRemoteIdStatus(const RemoteIdStatus& value);
```

- [ ] **Step 2: 修改 `state_store.cpp` 的实现**

打开 `src/state/state_store.cpp`，找到已有的：

```cpp
void StateStore::UpdateMotorPwm(const MotorPwm& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.motor_pwm = value;
}
```

**这段保留不动**（Task 2 才会删除），在它之后加：

```cpp
void StateStore::UpdateMotorPwmLow(std::uint8_t duty0, std::uint8_t duty1, bool run_state,
                                     std::uint8_t speed_level) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!state_.motor_pwm.has_value()) {
    state_.motor_pwm = MotorPwm{};
  }
  state_.motor_pwm->duty_percent[0] = duty0;
  state_.motor_pwm->duty_percent[1] = duty1;
  state_.motor_pwm->run_state = run_state;
  state_.motor_pwm->speed_level = speed_level;
}

void StateStore::UpdateMotorPwmHigh(std::uint8_t duty2, std::uint8_t duty3, bool run_state,
                                      std::uint8_t speed_level) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!state_.motor_pwm.has_value()) {
    state_.motor_pwm = MotorPwm{};
  }
  state_.motor_pwm->duty_percent[2] = duty2;
  state_.motor_pwm->duty_percent[3] = duty3;
  state_.motor_pwm->run_state = run_state;
  state_.motor_pwm->speed_level = speed_level;
}
```

紧跟 `void StateStore::UpdateEnvHumidity(...)` 的实现之后加：

```cpp
void StateStore::UpdateLoraStatus(const LoraStatus& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.lora_status = value;
}

void StateStore::UpdateRemoteIdStatus(const RemoteIdStatus& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.remote_id_status = value;
}
```

- [ ] **Step 3: 更新 `tests/test_state_store.cpp` 里因为删除 `UpdateMotorPwm` 而编译失败的既有测试**

打开 `tests/test_state_store.cpp`，把 `TEST_CASE("扩展遥测字段的Update各自独立,不影响其他字段")` 里这一段：

```cpp
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
```

改成：

```cpp
  state::Battery2Status bat2{12600, 80, false};
  state::GnssSat sat{12, 8, 10, 6};
  state::EnvHumidity hum{535};

  store.UpdateBattery2Status(bat2);
  store.UpdateMotorPwmLow(10, 20, true, 50);
  store.UpdateGnssSat(sat);
  store.UpdateEnvHumidity(hum);
  auto snapshot = store.Snapshot();

  REQUIRE(snapshot.battery2_status.has_value());
  CHECK(snapshot.battery2_status->voltage_mv == 12600);
  CHECK(snapshot.battery2_status->percent == 80);
  CHECK_FALSE(snapshot.battery2_status->low_voltage);

  REQUIRE(snapshot.motor_pwm.has_value());
  CHECK(snapshot.motor_pwm->duty_percent[0] == 10);
  CHECK(snapshot.motor_pwm->duty_percent[1] == 20);
  CHECK(snapshot.motor_pwm->run_state);
  CHECK(snapshot.motor_pwm->speed_level == 50);
```

（这个 TEST_CASE 剩下的部分——`gnss_sat`/`env_humidity`/`alarm_table`/`message_log` 的断言——不用改。）

- [ ] **Step 4: 新增 `MotorPwm`/`LoraStatus`/`RemoteIdStatus` 的正向测试**

在 `tests/test_state_store.cpp` 文件末尾加：

```cpp
TEST_CASE("UpdateMotorPwmLow只影响duty_percent的0-1号,UpdateMotorPwmHigh只影响2-3号,run_state/speed_level以最新一帧为准") {
  state::StateStore store;

  store.UpdateMotorPwmLow(10, 20, true, 50);
  auto after_low = store.Snapshot();
  REQUIRE(after_low.motor_pwm.has_value());
  CHECK(after_low.motor_pwm->duty_percent[0] == 10);
  CHECK(after_low.motor_pwm->duty_percent[1] == 20);
  CHECK(after_low.motor_pwm->duty_percent[2] == 0);  // MOTOR34还没到，零初始化占位
  CHECK(after_low.motor_pwm->run_state);
  CHECK(after_low.motor_pwm->speed_level == 50);

  store.UpdateMotorPwmHigh(30, 40, false, 60);
  auto after_high = store.Snapshot();
  REQUIRE(after_high.motor_pwm.has_value());
  CHECK(after_high.motor_pwm->duty_percent[0] == 10);  // 0-1号仍是UpdateMotorPwmLow写入的值
  CHECK(after_high.motor_pwm->duty_percent[1] == 20);
  CHECK(after_high.motor_pwm->duty_percent[2] == 30);
  CHECK(after_high.motor_pwm->duty_percent[3] == 40);
  CHECK_FALSE(after_high.motor_pwm->run_state);  // 两帧冗余拷贝,以最新一帧为准
  CHECK(after_high.motor_pwm->speed_level == 60);
}

TEST_CASE("UpdateLoraStatus和UpdateRemoteIdStatus各自独立写入") {
  state::StateStore store;
  state::LoraStatus lora{100, 7, true, 2};
  state::RemoteIdStatus rid{50, 3, 123456};

  store.UpdateLoraStatus(lora);
  store.UpdateRemoteIdStatus(rid);
  auto snapshot = store.Snapshot();

  REQUIRE(snapshot.lora_status.has_value());
  CHECK(snapshot.lora_status->loss_rate_x10 == 100);
  CHECK(snapshot.lora_status->node_id == 7);
  CHECK(snapshot.lora_status->present);
  CHECK(snapshot.lora_status->link_state == 2);

  REQUIRE(snapshot.remote_id_status.has_value());
  CHECK(snapshot.remote_id_status->location_count == 50);
  CHECK(snapshot.remote_id_status->error_count == 3);
  CHECK(snapshot.remote_id_status->last_success_ms == 123456);
}
```

- [ ] **Step 5: 编译并运行测试**

Run: `cmake --build build --target test_state_store && ./build/test_state_store`
Expected: 所有 TEST_CASE 通过，退出码 0，无编译警告/错误。

- [ ] **Step 6: Commit**

```bash
git add src/state/state_store.hpp src/state/state_store.cpp tests/test_state_store.cpp
git commit -m "feat: state_store同步固件字段变更(MotorPwm扩展+LoraStatus+RemoteIdStatus)"
```

---

### Task 2: extension_decoder 接入新字段字典

**Files:**
- Modify: `src/protocol/extension_decoder.cpp`
- Test: `tests/test_extension_decoder.cpp`

**Interfaces:**
- Consumes: `state::StateStore::UpdateMotorPwmLow/UpdateMotorPwmHigh/UpdateLoraStatus/UpdateRemoteIdStatus`（Task 1）。
- Produces: `protocol::DecodeExtensionAndStore` 现在认识 `HUMIDITY`（替代 `ENVHUM`）、`MOTOR12`/`MOTOR34`（替代 `MOTORPWM`）、`LORASTAT`、`RIDSTAT`（供 Task 3 main.cpp 的 `LogExtension` 读取 snapshot 里对应字段）。

- [ ] **Step 1: 删除 `MOTORPWM` 分支，`ENVHUM` 改名为 `HUMIDITY`**

打开 `src/protocol/extension_decoder.cpp`，删除整个：

```cpp
  if (name == "MOTORPWM") {
    state::MotorPwm pwm{};
    for (std::size_t i = 0; i < pwm.duty_percent.size(); ++i) {
      pwm.duty_percent[i] = static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFF);
    }
    store.UpdateMotorPwm(pwm);
    return true;
  }
```

把：

```cpp
  if (name == "ENVHUM") {
```

改成：

```cpp
  if (name == "HUMIDITY") {
```

**同时删除 `state_store.hpp`/`.cpp` 里的 `UpdateMotorPwm`**：Task 1 保留了这个方法（因为当时这里的 `MOTORPWM` 分支还在调用它），现在这个分支被删除了，`UpdateMotorPwm` 没有调用方了，一并删除。

打开 `src/state/state_store.hpp`，删除这一行（在 `UpdateMotorPwmLow`/`UpdateMotorPwmHigh` 声明之前）：

```cpp
  void UpdateMotorPwm(const MotorPwm& value);
```

打开 `src/state/state_store.cpp`，删除这一段（在 `UpdateMotorPwmLow` 实现之前）：

```cpp
void StateStore::UpdateMotorPwm(const MotorPwm& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.motor_pwm = value;
}
```

（这个分支内部逻辑不变，只改字符串。）

- [ ] **Step 2: 新增 `MOTOR12`/`MOTOR34`/`LORASTAT`/`RIDSTAT` 分支**

在 `if (name == "HUMIDITY") { ... return true; }` 分支之后、`return false;` 之前加：

```cpp
  if (name == "MOTOR12" || name == "MOTOR34") {
    const auto duty0 = static_cast<std::uint8_t>(bits & 0xFF);
    const auto duty1 = static_cast<std::uint8_t>((bits >> 8) & 0xFF);
    const bool run_state = ((bits >> 16) & 0x1) != 0;
    const auto speed_level = static_cast<std::uint8_t>((bits >> 24) & 0xFF);
    if (name == "MOTOR12") {
      store.UpdateMotorPwmLow(duty0, duty1, run_state, speed_level);
    } else {
      store.UpdateMotorPwmHigh(duty0, duty1, run_state, speed_level);
    }
    return true;
  }
  if (name == "LORASTAT") {
    state::LoraStatus lora{};
    lora.loss_rate_x10 = static_cast<std::uint16_t>(bits & 0xFFFF);
    lora.node_id = static_cast<std::uint8_t>((bits >> 16) & 0xFF);
    lora.present = ((bits >> 24) & 0x1) != 0;
    lora.link_state = static_cast<std::uint8_t>((bits >> 25) & 0x7);
    store.UpdateLoraStatus(lora);
    return true;
  }
  if (name == "RIDSTAT") {
    state::RemoteIdStatus rid{};
    rid.location_count = static_cast<std::uint16_t>(bits & 0xFFFF);
    rid.error_count = static_cast<std::uint16_t>((bits >> 16) & 0xFFFF);
    rid.last_success_ms = value.time_boot_ms;
    store.UpdateRemoteIdStatus(rid);
    return true;
  }
```

- [ ] **Step 3: 删除旧 `MOTORPWM` 测试，`ENVHUM` 测试改名**

打开 `tests/test_extension_decoder.cpp`，删除整个：

```cpp
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
```

把：

```cpp
TEST_CASE("ENVHUM解码保持原始x10刻度,不做单位换算") {
  mavlink_message_t msg = PackNamedValueInt("ENVHUM", /*value=*/535);
```

改成：

```cpp
TEST_CASE("HUMIDITY解码保持原始x10刻度,不做单位换算") {
  mavlink_message_t msg = PackNamedValueInt("HUMIDITY", /*value=*/535);
```

（TEST_CASE 内部剩余的断言不用改。）

- [ ] **Step 4: 新增 `MOTOR12`/`MOTOR34`/`LORASTAT`/`RIDSTAT` 测试**

在文件末尾（最后一个 `TEST_CASE` 之后）加：

```cpp
TEST_CASE("MOTOR12解码写入duty_percent的0-1号,run_state/speed_level一并写入") {
  constexpr std::int32_t kValue =
      static_cast<std::int32_t>(10u | (20u << 8) | (1u << 16) | (50u << 24));
  mavlink_message_t msg = PackNamedValueInt("MOTOR12", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.motor_pwm.has_value());
  CHECK(snapshot.motor_pwm->duty_percent[0] == 10);
  CHECK(snapshot.motor_pwm->duty_percent[1] == 20);
  CHECK(snapshot.motor_pwm->run_state);
  CHECK(snapshot.motor_pwm->speed_level == 50);
}

TEST_CASE("MOTOR34解码写入duty_percent的2-3号,不影响MOTOR12已写入的0-1号") {
  constexpr std::int32_t kLowValue =
      static_cast<std::int32_t>(10u | (20u << 8) | (1u << 16) | (50u << 24));
  constexpr std::int32_t kHighValue =
      static_cast<std::int32_t>(30u | (40u << 8) | (0u << 16) | (60u << 24));
  state::StateStore store;
  protocol::DecodeExtensionAndStore(PackNamedValueInt("MOTOR12", kLowValue), store);

  bool handled = protocol::DecodeExtensionAndStore(PackNamedValueInt("MOTOR34", kHighValue), store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.motor_pwm.has_value());
  CHECK(snapshot.motor_pwm->duty_percent[0] == 10);  // 0-1号仍是MOTOR12写入的值
  CHECK(snapshot.motor_pwm->duty_percent[1] == 20);
  CHECK(snapshot.motor_pwm->duty_percent[2] == 30);
  CHECK(snapshot.motor_pwm->duty_percent[3] == 40);
  CHECK_FALSE(snapshot.motor_pwm->run_state);  // 两帧冗余拷贝,以最新一帧为准
  CHECK(snapshot.motor_pwm->speed_level == 60);
}

TEST_CASE("LORASTAT解码拆出丢包率/节点ID/在位标志/链路状态") {
  constexpr std::int32_t kValue =
      static_cast<std::int32_t>(100u | (7u << 16) | (1u << 24) | (2u << 25));
  mavlink_message_t msg = PackNamedValueInt("LORASTAT", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.lora_status.has_value());
  CHECK(snapshot.lora_status->loss_rate_x10 == 100);
  CHECK(snapshot.lora_status->node_id == 7);
  CHECK(snapshot.lora_status->present);
  CHECK(snapshot.lora_status->link_state == 2);
}

TEST_CASE("RIDSTAT解码拆出位置广播成功计数/错误计数,time_boot_ms存入last_success_ms") {
  mavlink_message_t msg{};
  constexpr std::int32_t kValue = static_cast<std::int32_t>(50u | (3u << 16));
  mavlink_msg_named_value_int_pack(kSystemId, kComponentId, &msg, /*time_boot_ms=*/123456,
                                    "RIDSTAT", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.remote_id_status.has_value());
  CHECK(snapshot.remote_id_status->location_count == 50);
  CHECK(snapshot.remote_id_status->error_count == 3);
  CHECK(snapshot.remote_id_status->last_success_ms == 123456);
}
```

- [ ] **Step 5: 编译运行测试（含 R6 回归确认）**

Run: `cmake --build build --target test_extension_decoder && ctest --test-dir build -R extension_decoder --output-on-failure`
Expected: 全部 TEST_CASE 通过，无警告无错误。**同时确认既有的 `GNSS_SAT`/`BAT2STAT`/`MODSTAT0`/`MODSTAT1`/TUNNEL/身份帧测试（这次没有改代码）仍然全绿**——这就是 R6 回归确认，不需要额外写代码。

- [ ] **Step 6: Commit**

```bash
git add src/protocol/extension_decoder.cpp tests/test_extension_decoder.cpp
git commit -m "feat: extension_decoder同步固件字段字典(HUMIDITY/MOTOR12+34/LORASTAT/RIDSTAT)"
```

---

### Task 3: main.cpp 打印同步

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `state::TelemetryState` 的 `motor_pwm`（新字段 `run_state`/`speed_level`）、`lora_status`、`remote_id_status`（Task 1）；`protocol::DecodeExtensionAndStore` 现在也认识 `HUMIDITY`/`MOTOR12`/`MOTOR34`/`LORASTAT`/`RIDSTAT`（Task 2）。
- Produces: 无（集成层最后一站）。

- [ ] **Step 1: `LogExtension` 里更新电机打印、`HUMIDITY` 改名、新增 `LORASTAT`/`RIDSTAT` 打印**

打开 `src/main.cpp`，把：

```cpp
      if (snapshot.motor_pwm) {
        std::cout << "MOTORPWM: [0]=" << static_cast<int>(snapshot.motor_pwm->duty_percent[0])
                  << std::endl;
      }
```

改成：

```cpp
      if (snapshot.motor_pwm) {
        std::cout << "MOTOR: duty=[" << static_cast<int>(snapshot.motor_pwm->duty_percent[0])
                  << "," << static_cast<int>(snapshot.motor_pwm->duty_percent[1]) << ","
                  << static_cast<int>(snapshot.motor_pwm->duty_percent[2]) << ","
                  << static_cast<int>(snapshot.motor_pwm->duty_percent[3])
                  << "] run_state=" << snapshot.motor_pwm->run_state
                  << " speed_level=" << static_cast<int>(snapshot.motor_pwm->speed_level)
                  << std::endl;
      }
```

把：

```cpp
      if (snapshot.env_humidity) {
        std::cout << "ENVHUM: relative_humidity_x10=" << snapshot.env_humidity->relative_humidity_x10
                  << std::endl;
      }
```

改成：

```cpp
      if (snapshot.env_humidity) {
        std::cout << "HUMIDITY: relative_humidity_x10=" << snapshot.env_humidity->relative_humidity_x10
                  << std::endl;
      }
      if (snapshot.lora_status) {
        std::cout << "LORASTAT: loss_rate_x10=" << snapshot.lora_status->loss_rate_x10
                  << " node_id=" << static_cast<int>(snapshot.lora_status->node_id)
                  << " present=" << snapshot.lora_status->present
                  << " link_state=" << static_cast<int>(snapshot.lora_status->link_state)
                  << std::endl;
      }
      if (snapshot.remote_id_status) {
        std::cout << "RIDSTAT: location_count=" << snapshot.remote_id_status->location_count
                  << " error_count=" << snapshot.remote_id_status->error_count
                  << " last_success_ms=" << snapshot.remote_id_status->last_success_ms
                  << std::endl;
      }
```

（两处改动都在 `case MAVLINK_MSG_ID_NAMED_VALUE_INT:` 分支内，位置不变。）

- [ ] **Step 2: 编译并运行完整测试套件确认没有回归**

Run: `cmake --build build --target cns_rpi`
Expected: 编译成功，无警告无错误。

Run: `ctest --test-dir build --output-on-failure`
Expected: 全部测试通过（`mavlink_link`/`app_config`/`state_store`/`telemetry_decoder`/`extension_decoder`/`identity` 六个测试套件全绿）。

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "feat: main.cpp打印同步固件字段字典变更"
```

---

### Task 4: 文档同步

**Files:**
- Modify: `docs/V1设计文档.md`
- Modify: `docs/固件对接-数据格式.md`
- Modify: `docs/MAVLink消息清单.md`

**Interfaces:**
- Consumes: 无。
- Produces: 无（文档任务）。

- [ ] **Step 1: 更新 `docs/V1设计文档.md` §4.1 字段字典**

打开 `docs/V1设计文档.md`，找到这一行（在"此前版本的文档把这两条自定义帧的 `name` 猜成了..."那句下面）：

```
| `name`（wire 上实际字符串） | value（int32）编码 |
|---|---|
| `MODSTAT0` | 模块 0-7 的状态，每模块 4 bit，LSB 在前：`(state & 0xF) << (i*4)` |
| `MODSTAT1` | 模块 8-13 的状态，编码同上（只有 6 个有效模块，高 2 个 nibble 恒为 0） |
| `BAT2STAT` | bit0-15：电压 mV（uint16）；bit16-23：电量百分比；bit24：低电压标志（0/1） |
| `MOTORPWM` | 最多 4 个电机的占空比，每个 1 字节：byte0..byte3 = 电机 0..3 的 duty_percent |
| `GNSS_SAT` | bit0-7：GPS 可见数；bit8-15：北斗可见数；bit16-23：GPS 使用数；bit24-31：北斗使用数 |
| `ENVHUM` | 相对湿度 × 10（如 535 = 53.5%），取值 0-1000 |
| `LORASUM` | LoRa 远程查看租约摘要（active_viewer/lease_id/remaining_s），主从机之间用，跟 RPi 关系不大，可以不接 |
```

改成：

```
**2026-07-07 跟固件 `Formal_Framework` PR#1 同步后订正**：固件已把 `ENVHUM`→`HUMIDITY`、单条 `MOTORPWM`→`MOTOR12`/`MOTOR34` 双帧，并新增 2 条 RPi 专属遥测（`LORASTAT`/`RIDSTAT`，只发 USART1，不上 LoRa）。下表已按最新固件源码更新，这是 M3b 字段字典的订正，不是新里程碑。

| `name`（wire 上实际字符串） | value（int32）编码 |
|---|---|
| `MODSTAT0` | 模块 0-7 的状态，每模块 4 bit，LSB 在前：`(state & 0xF) << (i*4)` |
| `MODSTAT1` | 模块 8-13 的状态，编码同上（只有 6 个有效模块，高 2 个 nibble 恒为 0） |
| `BAT2STAT` | bit0-15：电压 mV（uint16）；bit16-23：电量百分比；bit24：低电压标志（0/1） |
| `MOTOR12` | `[0:7]`电机1占空% `[8:15]`电机2占空% `[16]`run_state(0/1) `[24:31]`speed_level% |
| `MOTOR34` | `[0:7]`电机3占空% `[8:15]`电机4占空% `[16]`run_state `[24:31]`speed_level（`run_state`/`speed_level` 两帧携带同一份整机状态的冗余拷贝） |
| `GNSS_SAT` | bit0-7：GPS 可见数；bit8-15：北斗可见数；bit16-23：GPS 使用数；bit24-31：北斗使用数 |
| `HUMIDITY` | 相对湿度 × 10（如 535 = 53.5%），取值 0-1000（此前叫 `ENVHUM`，已按固件实际改名） |
| `LORASTAT` | RPi 专属，只发 USART1：`[0:15]`丢包率×10 `[16:23]`LoRa节点ID `[24]`LoRa模块在位标志 `[25:27]`LoRa链路状态枚举(`Px4Lite_State_t`) |
| `RIDSTAT` | RPi 专属，只发 USART1：`[0:15]`位置广播成功计数低16位(增量) `[16:31]`编码/提交错误计数低16位(增量)，`time_boot_ms`=RemoteID最近一次成功提交时间 |
| `LORASUM` | LoRa 远程查看租约摘要（active_viewer/lease_id/remaining_s），主从机之间用，跟 RPi 关系不大，可以不接 |
```

- [ ] **Step 2: 更新 `docs/固件对接-数据格式.md` 第二部分**

打开 `docs/固件对接-数据格式.md`，找到 §2.2 里这一段表格：

```
| `name`（wire 上实际字符串） | value（int32，按位打包） | 备注 |
|---|---|---|
| `MODSTAT0` | 模块 0-7 的状态，每模块 4 bit：`(state & 0xF) << (i*4)`，`i`=模块序号 0-7 | **每模块 4 bit，模块 0 在最低 4 bit（LSB 端）**，模块 7 在最高 4 bit |
| `MODSTAT1` | 模块 8-13 的状态，编码方式同上，`i`=0 对应模块 8 | 只有 6 个有效模块（8-13），value 的高 2 个 nibble（对应 i=6,7）恒为 0 |
| `BAT2STAT` | bit0-15：电压 mV（`uint16_t`）；bit16-23：电量百分比；bit24：低电压标志（0/1） | bit25-31 未使用 |
| `MOTORPWM` | 最多 4 个电机占空比，每个 1 字节：byte0..byte3 = 电机 0..3 的 `duty_percent` | **byte0 是最低字节（bit0-7），对应电机 0**，逐字节递增对应电机序号递增 |
| `GNSS_SAT` | bit0-7：GPS 可见数；bit8-15：北斗可见数；bit16-23：GPS 使用数；bit24-31：北斗使用数 | 4 个字节各占 1 段，顺序固定为"GPS可见→北斗可见→GPS使用→北斗使用" |
| `ENVHUM` | 相对湿度 × 10（如 535 = 53.5%），取值范围 0-1000 | 整个 `int32` 直接存该整数值，不再拆分 bit 段 |
| `LORASUM` | LoRa 远程查看租约摘要（`active_viewer`/`lease_id`/`remaining_s`） | 主从机之间用的字段，RPi 侧不解码这个 name（未在 `extension_decoder.cpp` 里处理），可以不接入新链路 |
```

改成：

```
| `name`（wire 上实际字符串） | value（int32，按位打包） | 备注 |
|---|---|---|
| `MODSTAT0` | 模块 0-7 的状态，每模块 4 bit：`(state & 0xF) << (i*4)`，`i`=模块序号 0-7 | **每模块 4 bit，模块 0 在最低 4 bit（LSB 端）**，模块 7 在最高 4 bit |
| `MODSTAT1` | 模块 8-13 的状态，编码方式同上，`i`=0 对应模块 8 | 只有 6 个有效模块（8-13），value 的高 2 个 nibble（对应 i=6,7）恒为 0 |
| `BAT2STAT` | bit0-15：电压 mV（`uint16_t`）；bit16-23：电量百分比；bit24：低电压标志（0/1） | bit25-31 未使用 |
| `MOTOR12` | `[0:7]`电机1占空% `[8:15]`电机2占空% `[16]`run_state(0/1) `[24:31]`speed_level% | 替代原单条 `MOTORPWM`，每帧只管2路电机 |
| `MOTOR34` | `[0:7]`电机3占空% `[8:15]`电机4占空% `[16]`run_state `[24:31]`speed_level | `run_state`/`speed_level` 跟 `MOTOR12` 帧携带同一份整机状态的冗余拷贝，两帧值相同 |
| `GNSS_SAT` | bit0-7：GPS 可见数；bit8-15：北斗可见数；bit16-23：GPS 使用数；bit24-31：北斗使用数 | 4 个字节各占 1 段，顺序固定为"GPS可见→北斗可见→GPS使用→北斗使用" |
| `HUMIDITY` | 相对湿度 × 10（如 535 = 53.5%），取值范围 0-1000 | 整个 `int32` 直接存该整数值，不再拆分 bit 段；此前叫 `ENVHUM`，已按固件实际改名 |
| `LORASTAT` | `[0:15]`丢包率×10 `[16:23]`LoRa节点ID `[24]`LoRa模块在位标志 `[25:27]`LoRa链路状态枚举(`Px4Lite_State_t`) | RPi 专属，只发 USART1，不上 LoRa |
| `RIDSTAT` | `[0:15]`位置广播成功计数低16位(增量) `[16:31]`编码/提交错误计数低16位(增量) | RPi 专属，只发 USART1；`time_boot_ms`=RemoteID最近一次成功提交时间 |
| `LORASUM` | LoRa 远程查看租约摘要（`active_viewer`/`lease_id`/`remaining_s`） | 主从机之间用的字段，RPi 侧不解码这个 name（未在 `extension_decoder.cpp` 里处理），可以不接入新链路 |
```

还要把 §2.1 里这句：

```
并且已经与 RPi 侧 `src/protocol/extension_decoder.cpp` 的实现逐字段核对一致（`MODSTAT0`/`MODSTAT1` 的 4bit 打包顺序、`BAT2STAT` 的 bit 划分、`MOTORPWM`/`GNSS_SAT` 的字节顺序、两种 `TUNNEL payload_type` 的表头+行字节布局，均已核对，没有偏差）。
```

改成：

```
并且已经与 RPi 侧 `src/protocol/extension_decoder.cpp` 的实现逐字段核对一致（`MODSTAT0`/`MODSTAT1` 的 4bit 打包顺序、`BAT2STAT` 的 bit 划分、`MOTOR12`/`MOTOR34`/`GNSS_SAT` 的字节顺序、两种 `TUNNEL payload_type` 的表头+行字节布局，均已核对，没有偏差）。**2026-07-07 补充**：固件 PR#1 落地后 `ENVHUM`→`HUMIDITY`、`MOTORPWM`→`MOTOR12`/`MOTOR34`，并新增 `LORASTAT`/`RIDSTAT` 两条 RPi 专属遥测，本文档表格已同步更新，RPi 侧解码同一次改动里已跟上。
```

- [ ] **Step 3: 更新 `docs/MAVLink消息清单.md` 第2节**

打开 `docs/MAVLink消息清单.md`，找到第2节的表格：

```
| `MODSTAT0` / `MODSTAT1` | 各 0.125 Hz（8000ms），两者交替发送 | 14 个模块的状态位图（4bit/模块） |
| `BAT2STAT` | 0.125 Hz（8000ms） | 电压(mV)/电量(%)/低电压标志 |
| `MOTORPWM` | 0.2 Hz（5000ms） | 4 个电机占空比(%) |
| `GNSS_SAT` | 0.1 Hz（10000ms） | GPS/北斗可见数与使用数 |
| `ENVHUM` | 0.125 Hz（8000ms） | 相对湿度 |
```

改成：

```
| `MODSTAT0` / `MODSTAT1` | 各 0.125 Hz（8000ms），两者交替发送 | 14 个模块的状态位图（4bit/模块） |
| `BAT2STAT` | 0.125 Hz（8000ms） | 电压(mV)/电量(%)/低电压标志 |
| `MOTOR12` / `MOTOR34` | 各 0.2 Hz（5000ms），两者交替发送（替代原单条 `MOTORPWM`） | 每帧2路电机占空比(%)+run_state+speed_level |
| `GNSS_SAT` | 0.1 Hz（10000ms） | GPS/北斗可见数与使用数 |
| `HUMIDITY` | 0.125 Hz（8000ms） | 相对湿度（此前叫 `ENVHUM`，已按固件实际改名） |
| `LORASTAT` | 1000ms，RPi 专属 | LoRa 链路状态（丢包率/节点ID/在位/链路状态） |
| `RIDSTAT` | 1000ms，RPi 专属 | RemoteID 广播状态（位置/错误计数、最近成功提交时间） |
```

- [ ] **Step 4: Commit**

```bash
git add docs/V1设计文档.md "docs/固件对接-数据格式.md" docs/MAVLink消息清单.md
git commit -m "docs: 字段字典同步固件Formal_Framework PR#1(HUMIDITY/MOTOR12+34/LORASTAT/RIDSTAT)"
```

---

## 完成后（不属于本计划任务，供执行者知悉）

- 真机验证：按 M2/M3a/M3b/M3c 的既定模式，4 个任务全部完成后同步到 `dcdw@192.168.11.4`，`rm -rf build && cmake -B build -S . && cmake --build build`，`ctest --test-dir build --output-on-failure`，确认零警告零错误、全部测试通过。
- 这次改动不影响 M3c 身份帧解码（`OPEN_DRONE_ID_*`）和 TUNNEL 告警表/日志解码——固件侧已确认这两块 wire 格式跟 RPi 现有实现逐字节一致，不需要动。
