# 本地飞控下发测试工具实施计划

> **供智能体执行：** 必须使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans`，逐项执行并在每项后审查。所有步骤使用复选框跟踪。

**目标：** 新增独立命令行工具 `cns_control_test`，使用与 MQTT 控制入口相同的 JSON 和生产控制模块，在不连接服务器的情况下完成 dry-run 或直接通过 UART 向 STM32 下发 MAVLink 控制帧并输出 ACK JSON。

**架构：** CLI 参数、输入读取和 dry-run JSON 放在不依赖硬件的 `control_test_cli` 模块中；真实 UART 编排只放在独立可执行文件入口。工具复用 `control_command::Parse`、`EncodeCommandLong`、`ControlTransaction`、`ObserveFlightControllerHeartbeat`、`IsExpectedCommandAck` 和 `uart::MavlinkLink`，不复制生产业务规则，也不修改 `cns_rpi` 主程序。

**技术栈：** C++23、CMake、nlohmann/json、官方 MAVLink C headers、doctest、现有 UART 封装。

## 全局约束

- 所有文档、公开注释、诊断信息和回答使用中文。
- 目标平台为 Raspberry Pi 5、Debian trixie、ARM64，保持 `-std=c++23 -Wall -Wextra`。
- 默认 dry-run；只有显式 `--send` 才允许打开 UART 和真实发送。
- 控制 JSON 与 MQTT `control/set` 完全一致，不增加第二套命令格式。
- RPi 控制来源为“已学习的 STM32 动态 `sysid` / `compid=191`”；
  STM32 端点按 `type=18`、`compid=193`、动态 `sysid=1..250` 学习。
- 自动化测试不得依赖服务器、MQTT、STM32、5G 模块或 SIM 卡。
- 不修改生成的 `src/mavlink/` 目录，不增加第三方依赖。
- 真机执行电机、起飞、降落或急停前必须拆除桨叶或断开电机动力。

---

## 文件结构

| 文件 | 职责 |
|---|---|
| `src/control_test/control_test_cli.hpp` | 声明 CLI 参数、错误类型、参数解析、输入读取和 dry-run JSON 接口 |
| `src/control_test/control_test_cli.cpp` | 实现不依赖 UART/MQTT 的纯逻辑 |
| `src/control_test/main.cpp` | 配置加载、串口打开、STM32 端点学习、发送、ACK/超时循环和退出码 |
| `tests/test_control_test_cli.cpp` | CLI、文件读取、互斥规则和 dry-run payload 单元测试 |
| `docs/本地飞控下发测试工具.md` | 操作说明、安全限制、输出和真机验收步骤 |
| `CMakeLists.txt` | 将纯逻辑加入 core，增加工具和测试目标 |

---

### 任务 1：实现 CLI 参数和输入读取纯逻辑

**文件：**

- 新建：`src/control_test/control_test_cli.hpp`
- 新建：`src/control_test/control_test_cli.cpp`
- 新建：`tests/test_control_test_cli.cpp`
- 修改：`CMakeLists.txt`

**接口：**

- 产出：`control_test::ParseArguments(std::span<const std::string_view>) -> std::expected<CliOptions, CliError>`。
- 产出：`control_test::LoadPayload(const CliOptions&) -> std::expected<std::string, CliError>`。
- `CliOptions` 包含 `bool send`、`std::filesystem::path config_path`、`std::optional<std::string> inline_payload`、`std::optional<std::filesystem::path> payload_file`。
- `CliError` 包含稳定的 `code` 和中文 `message`，供入口映射为退出码 2。

- [ ] **步骤 1：先写 CLI 失败测试**

在 `tests/test_control_test_cli.cpp` 中建立 doctest 主入口，并覆盖以下实际参数：

```cpp
TEST_CASE("直接JSON默认进入dry-run") {
  const std::array<std::string_view, 2> args{
      "config/config.json",
      R"({"command_id":"local-1","command":"takeoff","parameters":{}})"};
  auto result = control_test::ParseArguments(args);
  REQUIRE(result.has_value());
  CHECK_FALSE(result->send);
  CHECK(result->config_path == "config/config.json");
  REQUIRE(result->inline_payload.has_value());
}

TEST_CASE("send和file位置无关") {
  const std::array<std::string_view, 4> args{
      "--file", "/tmp/control.json", "--send", "config/config.json"};
  auto result = control_test::ParseArguments(args);
  REQUIRE(result.has_value());
  CHECK(result->send);
  CHECK(result->payload_file == "/tmp/control.json");
}

TEST_CASE("直接JSON和file互斥") {
  const std::array<std::string_view, 4> args{
      "config/config.json", "{}", "--file", "/tmp/control.json"};
  auto result = control_test::ParseArguments(args);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == "conflicting_payload_sources");
}
```

同时覆盖：缺配置路径、缺 payload、`--file` 缺值、未知选项、两个普通 JSON 参数和空文件。

- [ ] **步骤 2：注册测试目标并确认 RED**

在 `CMakeLists.txt` 中先增加：

```cmake
add_executable(test_control_test_cli tests/test_control_test_cli.cpp)
target_link_libraries(test_control_test_cli PRIVATE cns_rpi_core)
add_test(NAME control_test_cli COMMAND test_control_test_cli)
```

运行：

```bash
cmake -S . -B build
cmake --build build --target test_control_test_cli -j2
```

预期：因 `control_test/control_test_cli.hpp` 或接口尚不存在而编译失败。

- [ ] **步骤 3：实现最小头文件**

在 `control_test_cli.hpp` 中定义：

```cpp
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace control_test {

struct CliOptions {
  bool send = false;
  std::filesystem::path config_path;
  std::optional<std::string> inline_payload;
  std::optional<std::filesystem::path> payload_file;
};

struct CliError {
  std::string code;
  std::string message;
};

std::expected<CliOptions, CliError> ParseArguments(
    std::span<const std::string_view> arguments);
std::expected<std::string, CliError> LoadPayload(const CliOptions& options);

}  // namespace control_test
```

- [ ] **步骤 4：实现参数解析和文件读取**

解析规则固定为：

- `--send` 是无值开关，可出现在任意位置且重复出现报 `duplicate_option`；
- `--file <path>` 消耗紧随其后的一个参数；
- 第一个非选项参数是配置路径；
- 第二个非选项参数是直接 JSON；
- 直接 JSON 与 `--file` 同时存在报 `conflicting_payload_sources`；
- 未知 `--` 选项报 `unknown_option`；
- `LoadPayload` 直接返回 inline 字符串，或以二进制输入流读取整个文件；打不开报 `payload_file_unreadable`，空文件报 `empty_payload`。

把 `src/control_test/control_test_cli.cpp` 加入 `cns_rpi_core` 源文件列表。

- [ ] **步骤 5：运行定向测试并提交**

运行：

```bash
cmake --build build --target test_control_test_cli -j2
ctest --test-dir build -R '^control_test_cli$' --output-on-failure
```

预期：全部 CLI 测试通过且无 `-Wall/-Wextra` 警告。

提交：

```bash
git add CMakeLists.txt src/control_test/control_test_cli.hpp \
  src/control_test/control_test_cli.cpp tests/test_control_test_cli.cpp
git commit -m "feat: add local control test CLI parsing"
```

---

### 任务 2：实现 dry-run JSON

**文件：**

- 修改：`src/control_test/control_test_cli.hpp`
- 修改：`src/control_test/control_test_cli.cpp`
- 修改：`tests/test_control_test_cli.cpp`

**接口：**

- 消费：`control_command::ControlCommand` 和 `EncodeCommandLong()`。
- 产出：`control_test::BuildDryRunResult(const ControlCommand&) -> nlohmann::json`。
- dry-run 使用占位目标 `target_system=0`、`target_component=193` 完成编码/解码；输出不声明已发现真实 STM32 端点。

- [ ] **步骤 1：写 dry-run 失败测试**

```cpp
TEST_CASE("dry-run输出实际MAVLink映射但不伪造目标sysid") {
  auto command = control_command::Parse(
      R"({"command_id":"local-1","command":"set_motor_pwm","parameters":{"pwm_us":[1500,1510,1520,1530]}})");
  REQUIRE(command.has_value());

  const auto result = control_test::BuildDryRunResult(*command);

  CHECK(result["status"] == "dry_run");
  CHECK(result["command_id"] == "local-1");
  CHECK(result["mavlink_command"] == control_command::kSetMotorPwmUs);
  CHECK(result["target_system"] == nullptr);
  CHECK(result["target_component"] == 193);
  CHECK(result["params"] == nlohmann::json::array({1500, 1510, 1520, 1530, 0, 0, 0}));
}
```

- [ ] **步骤 2：运行测试确认 RED**

运行 `cmake --build build --target test_control_test_cli -j2`。

预期：因 `BuildDryRunResult` 未声明而编译失败。

- [ ] **步骤 3：实现 dry-run 构造**

调用：

```cpp
const auto message = control_command::EncodeCommandLong(
    command, 250, MAV_COMP_ID_ONBOARD_COMPUTER,
    0, control_command::kStm32Usart6ComponentId);
mavlink_command_long_t packet{};
mavlink_msg_command_long_decode(&message, &packet);
```

从解码后的 `packet.command`、`param1..param7` 构造 JSON。`target_system`
明确为 `null`，表示 dry-run 没有等待真实 HEARTBEAT；`target_component` 输出 193。

- [ ] **步骤 4：运行定向测试并提交**

运行：

```bash
cmake --build build --target test_control_test_cli -j2
ctest --test-dir build -R '^control_test_cli$' --output-on-failure
```

提交：

```bash
git add src/control_test/control_test_cli.hpp \
  src/control_test/control_test_cli.cpp tests/test_control_test_cli.cpp
git commit -m "feat: add local control dry run"
```

---

### 任务 3：实现独立 UART 下发工具

**文件：**

- 新建：`src/control_test/main.cpp`
- 修改：`CMakeLists.txt`

**接口：**

- 消费：任务 1/2 的 CLI 和 dry-run 接口。
- 消费：`config::LoadAppConfig`、`uart::MavlinkLink`、`ControlTransaction`、`control_endpoint`。
- 产出：可执行文件 `build/cns_control_test`。

- [ ] **步骤 1：写入口契约测试**

在 `tests/test_control_test_cli.cpp` 增加退出状态判定的纯函数测试，并在头文件声明：

```cpp
int ExitCodeForFinalAck(const nlohmann::json& ack);
```

测试：

```cpp
CHECK(control_test::ExitCodeForFinalAck({{"status", "accepted"}}) == 0);
CHECK(control_test::ExitCodeForFinalAck({{"status", "rejected"}}) == 1);
CHECK(control_test::ExitCodeForFinalAck({{"status", "timeout"}}) == 1);
CHECK(control_test::ExitCodeForFinalAck(nlohmann::json::object()) == 1);
```

运行定向构建，预期因函数缺失而失败；随后实现该纯函数并确认测试通过。

- [ ] **步骤 2：建立入口和退出码常量**

`src/control_test/main.cpp` 使用以下常量：

```cpp
// source system 使用从 STM32 HEARTBEAT 学习到的 endpoint.system_id。
constexpr std::uint8_t kControlComponentId = MAV_COMP_ID_ONBOARD_COMPUTER;
constexpr auto kHeartbeatWaitTimeout = std::chrono::seconds(5);
constexpr auto kCommandAckTimeout = std::chrono::seconds(2);
constexpr int kCommandFailed = 1;
constexpr int kUsageOrConfigError = 2;
constexpr int kUartError = 3;
```

`main()` 将 `argv[1..]` 转为 `std::vector<std::string_view>`，调用
`ParseArguments()`、`LoadPayload()` 和 `control_command::Parse()`。CLI/配置错误写
stderr 并返回 2；控制 JSON 拒绝使用 `BuildRejectedAck()` 写 stdout 并返回 1。

- [ ] **步骤 3：实现 dry-run 分支**

当 `options.send == false` 时：

```cpp
std::cout << control_test::BuildDryRunResult(*command).dump() << '\n';
return EXIT_SUCCESS;
```

该分支必须出现在 `LoadAppConfig()` 和 `MavlinkLink::Open()` 之前，从结构上保证
dry-run 不接触串口；配置路径只进行 CLI 形状校验，不读取文件。

- [ ] **步骤 4：实现 STM32 端点学习**

真实发送模式读取配置并打开串口。循环调用 `ReceiveMessage()`，每次将收到的帧传给：

```cpp
endpoint = control_command::ObserveFlightControllerHeartbeat(*message, endpoint);
```

达到 5 秒仍未学习端点时，stderr 输出“等待 STM32 HEARTBEAT 超时，未发送控制命令”，
返回 3。学习成功后打印动态 sysid、固定 compid 193、命令名和参数到 stderr。

- [ ] **步骤 5：实现发送和 ACK 循环**

创建 `ControlTransaction transaction(kCommandAckTimeout, 1)`，调用 `Submit()`，再按
学习到的 endpoint 编码并发送。UART 写失败时调用：

```cpp
transaction.HandleLocalFailure({
    .code = "uart_send_failed",
    .message = "控制命令发送到单片机失败",
});
```

ACK 循环每轮：

1. 接收 UART 帧；
2. 使用 `IsExpectedCommandAck()` 过滤；
3. 解码 `mavlink_command_ack_t`；
4. 调用 `HandleMavlinkAck(command, result, progress, result_param2, now)`；
5. 调用 `CheckTimeout(now)`；
6. `PendingAck()` 非空时将 JSON 写 stdout 并立即 `ConfirmAckPublished()`；
7. progress ACK 后继续等待；最终 ACK 或 timeout 后按 `ExitCodeForFinalAck()` 退出。

stdout 每个 JSON 独占一行，便于脚本逐行解析。不得把诊断文本写 stdout。

- [ ] **步骤 6：注册可执行目标并验证无服务器依赖**

在 `CMakeLists.txt` 增加：

```cmake
add_executable(cns_control_test src/control_test/main.cpp)
target_link_libraries(cns_control_test PRIVATE cns_rpi_core)
```

不得链接 `cns_rpi_mqtt`。运行：

```bash
cmake -S . -B build
cmake --build build --target cns_control_test test_control_test_cli -j2
./build/cns_control_test config/not-needed.json \
  '{"command_id":"local-1","command":"takeoff","parameters":{}}'
```

预期：构建成功；dry-run 返回 0、输出单行 `status=dry_run` JSON，即使配置路径不存在也不访问 UART。

- [ ] **步骤 7：提交 UART 工具**

```bash
git add CMakeLists.txt src/control_test/main.cpp \
  src/control_test/control_test_cli.hpp src/control_test/control_test_cli.cpp \
  tests/test_control_test_cli.cpp
git commit -m "feat: add standalone STM32 control test tool"
```

---

### 任务 4：补充操作文档

**文件：**

- 新建：`docs/本地飞控下发测试工具.md`
- 修改：`docs/V1设计文档.md`

**接口：**

- 产出：操作者可复制执行的 dry-run、直接 JSON、`--file`、真实发送和故障排查命令。

- [ ] **步骤 1：编写操作文档**

文档必须包含：

- JSON 只用于本地输入，UART 上发送的是 MAVLink 二进制 `COMMAND_LONG`；
- 完整命令示例和 shell 引号注意事项；
- `--file` 示例；
- 默认 dry-run 与 `--send` 的区别；
- stdout JSON、stderr 诊断和退出码表；
- `cns_rpi` 占用串口时先停止服务；
- 拆桨/断开动力警告；
- HEARTBEAT 5 秒和 ACK 2 秒超时；
- `IN_PROGRESS` 可能输出多行 JSON；
- 工具单次运行不提供跨进程幂等，重复 `--send` 会重复执行。

- [ ] **步骤 2：更新 V1 文档状态**

在 `docs/V1设计文档.md` 的 M6 条目后补充：本地联调可使用
`cns_control_test` 绕过 MQTT/服务器，但该工具不是生产控制入口，正式运行仍只接受
服务器路由后的 MQTT 控制命令。

- [ ] **步骤 3：检查文档并提交**

运行：

```bash
rg -n "待编写|待补充|占位内容" docs/本地飞控下发测试工具.md
git diff --check
```

预期：第一条无输出，第二条无格式错误。

提交：

```bash
git add docs/本地飞控下发测试工具.md docs/V1设计文档.md
git commit -m "docs: document local STM32 control testing"
```

---

### 任务 5：完整验证与真机交接

**文件：**

- 修改：`docs/本地飞控下发测试工具.md`（只记录实际执行结果）

**接口：**

- 产出：开发机完整验证记录，以及明确标记的树莓派/STM32 真机待验项。

- [ ] **步骤 1：完整构建和测试**

运行：

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
git diff --check
```

预期：构建退出 0、全部测试通过、diff check 无输出。

- [ ] **步骤 2：执行四类 dry-run**

分别运行 `set_motor_pwm`、`takeoff`、`land`、`emergency_stop`，确认：

- 每条命令退出码为 0；
- stdout 只有一行合法 JSON；
- MAVLink command 分别为 `31013`、`31091`、`31092`、`31090`；
- PWM 参数为输入的四路微秒值，其余参数为 0。

- [ ] **步骤 3：验证非法输入不访问硬件**

运行越界 PWM、未知命令、非法 JSON、直接 JSON 与 `--file` 冲突，确认返回 1 或 2，
并且不要求配置文件或 UART 存在。

- [ ] **步骤 4：在 Raspberry Pi ARM64 构建**

在树莓派工作副本运行与步骤 1 相同的配置、构建和测试命令。预期 ARM64 构建成功，
全部测试通过。此步骤不需要连接 STM32。

- [ ] **步骤 5：真机 UART 验收（需要用户准备硬件）**

只有在用户确认树莓派已连接 STM32、主程序已停止、电机动力已断开或桨叶已拆除后，
才执行设计文档第 9 节的真实 `--send` 验收。未满足条件时将此项记录为“待真机验证”，
不得用 dry-run 或单元测试代替真机成功结论。

- [ ] **步骤 6：最终审查与提交验证记录**

使用 `superpowers:requesting-code-review` 审查从计划基线到 HEAD 的完整 diff。修复所有
Critical/Important 问题并重新运行步骤 1。仅把实际运行的平台、命令和结果写入操作文档。

```bash
git add docs/本地飞控下发测试工具.md
git commit -m "docs: record local control tool verification"
```

如果没有新增验证记录，跳过空提交。
