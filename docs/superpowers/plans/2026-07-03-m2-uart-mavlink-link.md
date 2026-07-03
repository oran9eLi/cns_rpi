# M2：UART/MAVLink 收发帧层 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 RPi 能通过 UART 用官方 `mavlink_frame_char_buffer()`/编码函数双向收发完整、CRC 校验过的 MAVLink v2 帧——不解析帧内容，只验证帧同步和收发闭环。

**Architecture:** 新增 `uart/serial_port`（纯 termios 字节收发）+ `uart/mavlink_link`（在其上包一层帧同步/编码，内部拆出一个不依赖硬件的 `MavlinkFrameAssembler` 保证可单测）+ `config/app_config`（nlohmann/json 读取配置文件，`std::expected` 报错）。三者打包进新的 `cns_rpi_core` CMake 库，`cns_rpi` 可执行文件和测试可执行文件都链接它，避免测试把 `main()` 一起拖进去。

**Tech Stack:** C++23 / CMake / POSIX termios / 官方 `mavlink/c_library_v2`（已 vendor 在 `src/mavlink/`）/ `nlohmann/json`（apt `nlohmann-json3-dev`）/ `doctest`（apt `doctest-dev`，单头文件，`/usr/include/doctest/doctest.h`，走系统默认 include 路径，不需要额外 CMake 配置）。

## Global Constraints

- 语言标准：C++23（`CMAKE_CXX_STANDARD 23`），已在 `CMakeLists.txt` 里设置，不要改。
- 编译警告：`-Wall -Wextra` 常开，新增代码必须零警告；`src/mavlink/` 是 vendor 的第三方头文件，已经用 `SYSTEM` 方式排除在警告范围外，不要移除这个处理。
- 错误处理：`uart/`、`config/` 里"操作可能失败"的场景一律用 `std::expected<T, ErrorEnum>`，不抛异常（`docs/V1设计文档.md` §7 技术选型定的约定）。
- 注释：新增/修改的 `.hpp`/`.cpp` 用 Doxygen 风格中文注释——文件头写职责/层级/依赖边界，公开类和函数写入参/返回值/失败语义（`docs/协作规则.md` §3）。
- 提交信息格式：`<type>: <简短中文说明>`（`docs/协作规则.md` §2），本计划里每个任务末尾给的 commit message 就是最终要用的文案，不要再展开长描述。
- `src/mavlink/` 下的官方头文件只读，本计划任何任务都不修改它。
- 目录结构必须落在 `docs/V1设计文档.md` §8 已经画好的位置：`src/uart/`、`src/config/`、`tests/`。

---

## 背景速查（写代码前要知道的关键事实，来自设计 spec）

- `mavlink_frame_char_buffer(rxmsg, status, c, r_message, r_status)` 是调用方自带解析缓冲区的版本（`src/mavlink/protocol.h:67`），不依赖任何全局 channel 状态，返回值 `0`=帧不完整，`1`=`MAVLINK_FRAMING_OK`（好帧+CRC对），`2`=CRC坏——只有返回 `1` 才能安全地把 `r_message` 当成一条合法帧使用。
- 发送方向：先用 `mavlink_msg_<type>_pack(...)`（比如 `mavlink_msg_heartbeat_pack`，在 `src/mavlink/minimal/mavlink_msg_heartbeat.h:66`）打包并自动完成 CRC finalize，再用 `mavlink_msg_to_send_buffer(buf, &msg)`（`src/mavlink/mavlink_helpers.h:451`）序列化成要写到串口的字节，缓冲区要开够 `MAVLINK_MAX_PACKET_LEN`（`src/mavlink/mavlink_types.h:46`）。
- `common/mavlink.h` 会一路经 `common.h` → `../standard/standard.h` → `../minimal/minimal.h` 把 HEARTBEAT 相关的类型/函数/枚举（`MAV_TYPE_ONBOARD_CONTROLLER`、`MAV_AUTOPILOT_INVALID`、`MAV_STATE_ACTIVE`、`MAV_COMP_ID_ONBOARD_COMPUTER`）都带进来，只 `#include "common/mavlink.h"` 一个头文件就够，不用单独 include `minimal/*.h`。
- MAVLink v2 帧头固定 10 字节（`MAVLINK_NUM_HEADER_BYTES`，`src/mavlink/mavlink_types.h:40`），CRC 校验通不过时该帧直接被官方库丢弃，不会出现在 `r_message` 里。

## 一个关键设计决策（spec 没细化到这一层，这里定下来）

spec 第 5 节要求测试"字节序列用官方 encode 函数现造"、覆盖"合法帧/CRC坏/半帧拆两次读/垃圾字节前缀"——但这些测试不能依赖真实串口设备（CI/开发机上没有硬件）。如果把帧同步逻辑焊死在 `MavlinkLink`（内部直接读 `SerialPort`），测试就没法喂字节进去。

解法：把"喂字节 → 吐完整帧"这部分拆成一个不碰 I/O 的类 `MavlinkFrameAssembler`（放在 `uart/mavlink_link.hpp` 里，不新开文件，不违反 spec 定的文件布局），`MavlinkLink` 只是拿它包一层 `SerialPort::Read()`。单测直接测 `MavlinkFrameAssembler::Feed()`，不用碰硬件；`MavlinkLink::ReceiveMessage()`/`SendMessage()` 走真实串口的部分留给人工物理验证（Task 8）。

---

### Task 1: CMake 脚手架——引入 nlohmann_json 依赖 + 开启 CTest

**Files:**
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `find_package(nlohmann_json)` 成功后可用的 `nlohmann_json::nlohmann_json` target；`enable_testing()` 打开后，后续任务里的 `add_test()` 才会被 `ctest` 收集到。

- [ ] **Step 1: 编辑 CMakeLists.txt，加依赖声明和 CTest 开关**

在 `add_compile_options(-Wall -Wextra)` 之后、`add_executable(cns_rpi src/main.cpp)` 之前插入：

```cmake
find_package(nlohmann_json 3.2.0 REQUIRED)

enable_testing()
```

- [ ] **Step 2: 验证 CMake 能找到这个包并正常配置**

Run: `cmake -B build -S .`
Expected: 配置成功结束，输出里能看到 `nlohmann_json` 相关信息，没有 "Could not find" 报错。如果本机没装 `nlohmann-json3-dev`，先跑 `sudo apt install -y nlohmann-json3-dev doctest-dev` 再重试（这两个包最终也会进 `scripts/install_deps.sh`，见 Task 6，这里先手动装一下不阻塞开发）。

- [ ] **Step 3: 确认现有可执行文件仍能正常构建（没有破坏 M1 的产物）**

Run: `cmake --build build`
Expected: `cns_rpi` 编译成功，无警告无报错。

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "$(cat <<'EOF'
build: 引入nlohmann_json依赖并开启CTest
EOF
)"
```

---

### Task 2: `uart/serial_port` —— termios 串口字节收发封装

**Files:**
- Create: `src/uart/serial_port.hpp`
- Create: `src/uart/serial_port.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:
  - `enum class uart::UartError { kDeviceNotFound, kPermissionDenied, kConfigFailed, kReadError, kWriteError };`
  - `class uart::SerialPort`：`static std::expected<SerialPort, UartError> Open(const std::string& device, int baud);`、`std::expected<std::size_t, UartError> Read(std::span<std::uint8_t> buffer);`、`std::expected<std::size_t, UartError> Write(std::span<const std::uint8_t> data);`，只能移动不能拷贝。

**说明：这一层没有自动化单测。** 它是对 POSIX termios 的薄封装，脱离真实字符设备没法有意义地测（开一个不存在的设备只能测出"文件不存在"这种平凡分支，没有测试价值）。真正的收发正确性在 Task 8 的物理链路人工验证里用真实硬件确认。这一步只要求编译通过、警告清零。

- [ ] **Step 1: 写 `src/uart/serial_port.hpp`**

```cpp
#pragma once

/**
 * @file serial_port.hpp
 * @brief 串口字节收发的 termios 封装。
 *
 * @details
 * 只负责"打开一个字符设备路径、配置波特率/8N1/raw模式、阻塞读写字节"，
 * 不知道 MAVLink 是什么，不做任何帧同步。帧同步在 uart/mavlink_link.hpp 里。
 * 依赖边界：只依赖 POSIX termios，不包含 mavlink/、mqtt/ 等上层模块头文件。
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace uart {

/// 串口相关操作的失败原因。
enum class UartError {
  kDeviceNotFound,    ///< 设备路径不存在
  kPermissionDenied,  ///< 没有读写权限（比如不在 dialout 组）
  kConfigFailed,      ///< termios 配置失败，或波特率不在支持列表里
  kReadError,         ///< read() 系统调用失败
  kWriteError,        ///< write() 系统调用失败
};

/// 一个已打开、已配置好的串口。只能移动，不能拷贝（持有一个 fd）。
class SerialPort {
 public:
  /**
   * @brief 打开并配置一个串口。
   * @param device 字符设备路径，例如 "/dev/ttyUSB0"。
   * @param baud 波特率，支持 9600/19200/38400/57600/115200/230400，其余值返回 kConfigFailed。
   * @return 成功返回可用的 SerialPort；失败返回具体错误原因。
   */
  static std::expected<SerialPort, UartError> Open(const std::string& device, int baud);

  SerialPort(SerialPort&& other) noexcept;
  SerialPort& operator=(SerialPort&& other) noexcept;
  SerialPort(const SerialPort&) = delete;
  SerialPort& operator=(const SerialPort&) = delete;
  ~SerialPort();

  /**
   * @brief 读取当前可用的字节，最多填满 buffer。
   * @details 配置为 VMIN=0/VTIME=1（100ms），没有数据时最多阻塞 100ms 后返回 0，
   * 不会无限阻塞——这样调用方（uart::MavlinkLink 的轮询循环）能穿插做周期性发送，
   * 不必开线程。
   * @return 实际读到的字节数（可能是 0）；系统调用失败返回 kReadError。
   */
  std::expected<std::size_t, UartError> Read(std::span<std::uint8_t> buffer);

  /**
   * @brief 把 data 全部写入串口，内部处理短写（循环写完为止）。
   * @return 成功返回写入的字节数（等于 data.size()）；失败返回 kWriteError。
   */
  std::expected<std::size_t, UartError> Write(std::span<const std::uint8_t> data);

 private:
  explicit SerialPort(int fd) : fd_(fd) {}

  int fd_ = -1;
};

}  // namespace uart
```

- [ ] **Step 2: 写 `src/uart/serial_port.cpp`**

```cpp
/**
 * @file serial_port.cpp
 * @brief serial_port.hpp 的实现。
 */

#include "uart/serial_port.hpp"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace uart {

namespace {

/// 把 config.json 里的整数波特率映射成 termios 的 speed_t 常量。
/// 只收 config.example.json 和现有硬件实测会用到的几档，不做穷举——多了也用不上。
std::expected<speed_t, UartError> ToSpeed(int baud) {
  switch (baud) {
    case 9600:
      return B9600;
    case 19200:
      return B19200;
    case 38400:
      return B38400;
    case 57600:
      return B57600;
    case 115200:
      return B115200;
    case 230400:
      return B230400;
    default:
      return std::unexpected(UartError::kConfigFailed);
  }
}

}  // namespace

std::expected<SerialPort, UartError> SerialPort::Open(const std::string& device, int baud) {
  auto speed = ToSpeed(baud);
  if (!speed) {
    return std::unexpected(speed.error());
  }

  int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY);
  if (fd < 0) {
    switch (errno) {
      case ENOENT:
      case ENXIO:
        return std::unexpected(UartError::kDeviceNotFound);
      case EACCES:
      case EPERM:
        return std::unexpected(UartError::kPermissionDenied);
      default:
        return std::unexpected(UartError::kConfigFailed);
    }
  }

  termios tio{};
  if (::tcgetattr(fd, &tio) != 0) {
    ::close(fd);
    return std::unexpected(UartError::kConfigFailed);
  }

  ::cfmakeraw(&tio);
  ::cfsetispeed(&tio, *speed);
  ::cfsetospeed(&tio, *speed);
  // VMIN=0/VTIME=1：最多等 100ms，没数据也返回，不无限阻塞。
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 1;

  if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
    ::close(fd);
    return std::unexpected(UartError::kConfigFailed);
  }

  return SerialPort(fd);
}

SerialPort::SerialPort(SerialPort&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

SerialPort::~SerialPort() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

std::expected<std::size_t, UartError> SerialPort::Read(std::span<std::uint8_t> buffer) {
  ssize_t n = ::read(fd_, buffer.data(), buffer.size());
  if (n < 0) {
    if (errno == EINTR) {
      return std::size_t{0};
    }
    return std::unexpected(UartError::kReadError);
  }
  return static_cast<std::size_t>(n);
}

std::expected<std::size_t, UartError> SerialPort::Write(std::span<const std::uint8_t> data) {
  std::size_t total = 0;
  while (total < data.size()) {
    ssize_t n = ::write(fd_, data.data() + total, data.size() - total);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(UartError::kWriteError);
    }
    total += static_cast<std::size_t>(n);
  }
  return total;
}

}  // namespace uart
```

- [ ] **Step 3: 把新文件接进 CMake，建 `cns_rpi_core` 库**

编辑 `CMakeLists.txt`，把原来的：

```cmake
add_executable(cns_rpi src/main.cpp)

# src/mavlink 是从固件仓库 Third_Party/mavlink/ 原样同步的官方生成头文件（common/standard/minimal），
# 只读、不改；SYSTEM 让 -Wall -Wextra 不对这批第三方代码报警，只管我们自己写的代码。
target_include_directories(cns_rpi SYSTEM PRIVATE src/mavlink)
```

替换成：

```cmake
# cns_rpi_core：uart/、config/ 等和 main() 无关的业务代码单独成库，
# 这样 tests/ 下的单测能直接链接这些代码，不用把 main() 也一起拖进测试二进制。
add_library(cns_rpi_core
    src/uart/serial_port.cpp
)
target_include_directories(cns_rpi_core PUBLIC src)
# src/mavlink 是从固件仓库 Third_Party/mavlink/ 原样同步的官方生成头文件（common/standard/minimal），
# 只读、不改；SYSTEM 让 -Wall -Wextra 不对这批第三方代码报警，只管我们自己写的代码。
target_include_directories(cns_rpi_core SYSTEM PUBLIC src/mavlink)
target_link_libraries(cns_rpi_core PUBLIC nlohmann_json::nlohmann_json)

add_executable(cns_rpi src/main.cpp)
target_link_libraries(cns_rpi PRIVATE cns_rpi_core)
```

（`nlohmann_json` 现在还没人用，但 Task 4 的 `config/app_config.cpp` 会加进这个库里用到它，提前声明一次，免得后面每加一个源文件都要回来改这一行。）

- [ ] **Step 4: 验证编译零警告**

Run: `cmake -B build -S . && cmake --build build 2>&1 | tee /tmp/build.log && grep -i warning /tmp/build.log`
Expected: 构建成功；`grep warning` 没有任何输出（说明零警告）。

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/uart/serial_port.hpp src/uart/serial_port.cpp
git commit -m "$(cat <<'EOF'
feat: 新增串口termios封装(serial_port)
EOF
)"
```

---

### Task 3: `uart/mavlink_link` —— 帧同步（`MavlinkFrameAssembler`）+ 收发封装（`MavlinkLink`）

**Files:**
- Create: `src/uart/mavlink_link.hpp`
- Create: `src/uart/mavlink_link.cpp`
- Create: `tests/test_mavlink_link.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `uart::SerialPort`（`Open`/`Read`/`Write`，来自 Task 2）、`uart::UartError`。
- Produces:
  - `class uart::MavlinkFrameAssembler`：`std::optional<mavlink_message_t> Feed(std::span<const std::uint8_t> bytes);`——不碰硬件，纯状态机，单测直接测它。
  - `class uart::MavlinkLink`：`static std::expected<MavlinkLink, UartError> Open(const std::string& device, int baud);`、`std::optional<mavlink_message_t> ReceiveMessage();`、`bool SendMessage(const mavlink_message_t& message);`

- [ ] **Step 1: 写失败的测试 `tests/test_mavlink_link.cpp`**

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "common/mavlink.h"
#include "uart/mavlink_link.hpp"

namespace {

/// 用官方 pack/to_send_buffer 现造一条合法的 HEARTBEAT 帧字节序列，测试不手写帧格式。
std::vector<std::uint8_t> PackHeartbeatBytes() {
  mavlink_message_t msg{};
  mavlink_msg_heartbeat_pack(/*system_id=*/1, /*component_id=*/MAV_COMP_ID_ONBOARD_COMPUTER, &msg,
                              MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID,
                              /*base_mode=*/0, /*custom_mode=*/0, MAV_STATE_ACTIVE);
  std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> buffer{};
  std::uint16_t len = mavlink_msg_to_send_buffer(buffer.data(), &msg);
  return std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + len);
}

}  // namespace

TEST_CASE("完整合法帧一次性喂入能被正确解出") {
  uart::MavlinkFrameAssembler assembler;
  auto bytes = PackHeartbeatBytes();

  auto result = assembler.Feed(bytes);

  REQUIRE(result.has_value());
  CHECK(result->msgid == MAVLINK_MSG_ID_HEARTBEAT);

  mavlink_heartbeat_t decoded{};
  mavlink_msg_heartbeat_decode(&*result, &decoded);
  CHECK(decoded.type == MAV_TYPE_ONBOARD_CONTROLLER);
  CHECK(decoded.autopilot == MAV_AUTOPILOT_INVALID);
}

TEST_CASE("CRC被篡改的帧不会被当成合法帧返回") {
  uart::MavlinkFrameAssembler assembler;
  auto bytes = PackHeartbeatBytes();
  // MAVLink v2 头部固定10字节，第10个字节(索引10)是payload第一字节；
  // 篡改它能破坏CRC又不改变帧长度字段，帧结构仍完整、只是CRC校验会失败。
  bytes[10] ^= 0xFF;

  auto result = assembler.Feed(bytes);

  CHECK_FALSE(result.has_value());
}

TEST_CASE("一帧被拆成两次读取仍能拼出完整帧") {
  uart::MavlinkFrameAssembler assembler;
  auto bytes = PackHeartbeatBytes();
  std::size_t split = bytes.size() / 2;
  std::vector<std::uint8_t> first_half(bytes.begin(), bytes.begin() + split);
  std::vector<std::uint8_t> second_half(bytes.begin() + split, bytes.end());

  auto first_result = assembler.Feed(first_half);
  CHECK_FALSE(first_result.has_value());

  auto second_result = assembler.Feed(second_half);
  REQUIRE(second_result.has_value());
  CHECK(second_result->msgid == MAVLINK_MSG_ID_HEARTBEAT);
}

TEST_CASE("合法帧前混入垃圾字节不影响帧被正确解出") {
  uart::MavlinkFrameAssembler assembler;
  std::vector<std::uint8_t> bytes = {0x00, 0xFF, 0x12, 0x34};
  auto heartbeat = PackHeartbeatBytes();
  bytes.insert(bytes.end(), heartbeat.begin(), heartbeat.end());

  auto result = assembler.Feed(bytes);

  REQUIRE(result.has_value());
  CHECK(result->msgid == MAVLINK_MSG_ID_HEARTBEAT);
}
```

- [ ] **Step 2: 先只加测试文件到 CMake，运行确认失败（因为 `uart/mavlink_link.hpp` 还不存在）**

编辑 `CMakeLists.txt`，在 `add_executable(cns_rpi ...)` 之后加：

```cmake
add_executable(test_mavlink_link tests/test_mavlink_link.cpp)
target_link_libraries(test_mavlink_link PRIVATE cns_rpi_core)
add_test(NAME mavlink_link COMMAND test_mavlink_link)
```

Run: `cmake -B build -S . && cmake --build build`
Expected: 编译失败，报错找不到 `uart/mavlink_link.hpp`（`fatal error: uart/mavlink_link.hpp: No such file or directory`）。这一步就是确认测试确实在驱动新代码，不是空跑。

- [ ] **Step 3: 写 `src/uart/mavlink_link.hpp`**

```cpp
#pragma once

/**
 * @file mavlink_link.hpp
 * @brief UART 字节流到完整 MAVLink 帧的同步、CRC 校验、编码发送。
 *
 * @details
 * 只负责"凑出一条通过CRC校验的完整帧"和"把一条帧编码发出去"，不解析帧内部字段
 * 语义（那是 M3 protocol/ 层的事）。
 * MavlinkFrameAssembler 是纯状态机，不碰任何 I/O，方便脱离硬件单测；
 * MavlinkLink 组合它和 uart::SerialPort，是真正对外使用的收发入口。
 * 依赖边界：只依赖 uart/serial_port.hpp 和官方 mavlink/c_library_v2 头文件，
 * 不包含 mqtt/、payload/ 等下游模块头文件。
 */

#include <cstdint>
#include <optional>
#include <queue>
#include <span>

#include "common/mavlink.h"
#include "uart/serial_port.hpp"

namespace uart {

/**
 * @brief 把原始字节流喂给官方 mavlink_frame_char_buffer()，攒出完整、CRC校验过的帧。
 * @details 不做任何 I/O，纯状态机；测试直接构造它、喂合成字节序列，不需要真实串口。
 */
class MavlinkFrameAssembler {
 public:
  /**
   * @brief 喂入一批新到达的字节。
   * @details 内部先把 bytes 全部喂给官方解析状态机，凑出的完整帧（含之前调用里还没
   * 取走的）会排进内部队列；然后从队首弹出一条返回。也就是说即使这次调用本身没
   * 凑出新帧，只要队列里还有上次剩下的，也会被返回——调用方不会因为"这次没读到新
   * 字节"就丢失已经解出来但还没被取走的帧。
   * @return 有帧可取就返回它（并从内部队列弹出）；没有则返回 std::nullopt。
   */
  std::optional<mavlink_message_t> Feed(std::span<const std::uint8_t> bytes);

 private:
  mavlink_message_t rx_msg_{};
  mavlink_status_t status_{};
  std::queue<mavlink_message_t> pending_;
};

/// 组合 SerialPort + MavlinkFrameAssembler，是 uart/ 层对外暴露的收发入口。
class MavlinkLink {
 public:
  /**
   * @brief 打开串口并准备好帧同步状态。
   * @param device 字符设备路径，例如 "/dev/ttyUSB0"。
   * @param baud 波特率，参见 uart::SerialPort::Open。
   */
  static std::expected<MavlinkLink, UartError> Open(const std::string& device, int baud);

  MavlinkLink(MavlinkLink&&) noexcept = default;
  MavlinkLink& operator=(MavlinkLink&&) noexcept = default;
  MavlinkLink(const MavlinkLink&) = delete;
  MavlinkLink& operator=(const MavlinkLink&) = delete;

  /**
   * @brief 读取串口当前可用字节并尝试凑出一条完整帧。
   * @details 单次调用最多阻塞 100ms（见 SerialPort::Read 的 VTIME 配置），
   * 不会无限阻塞，调用方可以在循环里穿插做周期性发送。
   * @return 凑出帧就返回；没有则 std::nullopt。串口读失败时也返回 std::nullopt
   * （M2 阶段不做断线重连，那是 M7 的事）。
   */
  std::optional<mavlink_message_t> ReceiveMessage();

  /**
   * @brief 把一条已经 pack 好的帧编码并写入串口。
   * @param message 必须是已经用 mavlink_msg_*_pack 系列函数完成 CRC finalize 的帧。
   * @return 写入成功（字节数与编码长度一致）返回 true，否则 false。
   */
  bool SendMessage(const mavlink_message_t& message);

 private:
  explicit MavlinkLink(SerialPort&& port) : port_(std::move(port)) {}

  SerialPort port_;
  MavlinkFrameAssembler assembler_;
};

}  // namespace uart
```

- [ ] **Step 4: 写 `src/uart/mavlink_link.cpp`**

```cpp
/**
 * @file mavlink_link.cpp
 * @brief mavlink_link.hpp 的实现。
 */

#include "uart/mavlink_link.hpp"

#include <array>

namespace uart {

std::optional<mavlink_message_t> MavlinkFrameAssembler::Feed(std::span<const std::uint8_t> bytes) {
  for (std::uint8_t byte : bytes) {
    mavlink_message_t out_msg{};
    mavlink_status_t out_status{};
    std::uint8_t result = mavlink_frame_char_buffer(&rx_msg_, &status_, byte, &out_msg, &out_status);
    if (result == MAVLINK_FRAMING_OK) {
      pending_.push(out_msg);
    }
  }

  if (pending_.empty()) {
    return std::nullopt;
  }
  mavlink_message_t msg = pending_.front();
  pending_.pop();
  return msg;
}

std::expected<MavlinkLink, UartError> MavlinkLink::Open(const std::string& device, int baud) {
  auto port = SerialPort::Open(device, baud);
  if (!port) {
    return std::unexpected(port.error());
  }
  return MavlinkLink(std::move(*port));
}

std::optional<mavlink_message_t> MavlinkLink::ReceiveMessage() {
  std::array<std::uint8_t, 256> buffer{};
  auto n = port_.Read(buffer);
  std::size_t count = n.value_or(0);
  return assembler_.Feed(std::span<const std::uint8_t>(buffer.data(), count));
}

bool MavlinkLink::SendMessage(const mavlink_message_t& message) {
  std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> buffer{};
  std::uint16_t len = mavlink_msg_to_send_buffer(buffer.data(), &message);
  auto result = port_.Write(std::span<const std::uint8_t>(buffer.data(), len));
  return result.has_value() && *result == len;
}

}  // namespace uart
```

- [ ] **Step 5: 把新源文件加进 `cns_rpi_core` 库**

编辑 `CMakeLists.txt`，把：

```cmake
add_library(cns_rpi_core
    src/uart/serial_port.cpp
)
```

改成：

```cmake
add_library(cns_rpi_core
    src/uart/serial_port.cpp
    src/uart/mavlink_link.cpp
)
```

- [ ] **Step 6: 运行测试，确认全部通过**

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: 编译零警告；`ctest` 显示 `mavlink_link` 这个 test 通过（4 个 `TEST_CASE` 全绿）。

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/uart/mavlink_link.hpp src/uart/mavlink_link.cpp tests/test_mavlink_link.cpp
git commit -m "$(cat <<'EOF'
feat: 新增MAVLink帧同步与收发封装(mavlink_link)
EOF
)"
```

---

### Task 4: `config/app_config` —— JSON 配置加载

**Files:**
- Create: `src/config/app_config.hpp`
- Create: `src/config/app_config.cpp`
- Create: `tests/test_app_config.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:
  - `enum class config::ConfigError { kFileNotFound, kParseError, kMissingField, kInvalidValue };`
  - `struct config::SerialConfig { std::string device; int baud; };`
  - `struct config::MqttConfig { std::string broker_host; int broker_port; std::string client_id; std::string username; std::string password; std::string topic_prefix; int qos; int keepalive_seconds; };`
  - `struct config::LoggingConfig { std::string level; std::string file; };`
  - `struct config::AppConfig { SerialConfig serial; MqttConfig mqtt; LoggingConfig logging; };`
  - `std::expected<config::AppConfig, config::ConfigError> config::LoadAppConfig(const std::filesystem::path& path);`

- [ ] **Step 1: 写失败的测试 `tests/test_app_config.cpp`**

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "config/app_config.hpp"

namespace {

std::filesystem::path WriteTempConfig(const std::string& content) {
  auto path = std::filesystem::temp_directory_path() / "cns_rpi_test_config.json";
  std::ofstream out(path, std::ios::trunc);
  out << content;
  return path;
}

}  // namespace

TEST_CASE("完整合法配置文件能正确解析出serial/mqtt/logging字段") {
  auto path = WriteTempConfig(R"({
    "serial": {"device": "/dev/ttyUSB0", "baud": 115200},
    "mqtt": {"broker_host": "192.168.1.100", "broker_port": 1883, "client_id": "cns-rpi",
             "username": "", "password": "", "topic_prefix": "cns_rpi", "qos": 1, "keepalive_seconds": 60},
    "logging": {"level": "info", "file": ""}
  })");

  auto result = config::LoadAppConfig(path);

  REQUIRE(result.has_value());
  CHECK(result->serial.device == "/dev/ttyUSB0");
  CHECK(result->serial.baud == 115200);
  CHECK(result->mqtt.broker_host == "192.168.1.100");
  CHECK(result->mqtt.broker_port == 1883);
  CHECK(result->mqtt.qos == 1);
  CHECK(result->logging.level == "info");
}

TEST_CASE("配置文件不存在时返回kFileNotFound") {
  auto result = config::LoadAppConfig("/nonexistent/path/config.json");

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kFileNotFound);
}

TEST_CASE("JSON格式损坏时返回kParseError") {
  auto path = WriteTempConfig("{ not valid json ");

  auto result = config::LoadAppConfig(path);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kParseError);
}

TEST_CASE("缺少必需字段时返回kMissingField") {
  auto path = WriteTempConfig(R"({"serial": {"device": "/dev/ttyUSB0"}})");

  auto result = config::LoadAppConfig(path);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kMissingField);
}
```

- [ ] **Step 2: 把测试文件加进 CMake，运行确认失败**

编辑 `CMakeLists.txt`，加：

```cmake
add_executable(test_app_config tests/test_app_config.cpp)
target_link_libraries(test_app_config PRIVATE cns_rpi_core)
add_test(NAME app_config COMMAND test_app_config)
```

Run: `cmake -B build -S . && cmake --build build`
Expected: 编译失败，找不到 `config/app_config.hpp`。

- [ ] **Step 3: 写 `src/config/app_config.hpp`**

```cpp
#pragma once

/**
 * @file app_config.hpp
 * @brief 读取 config/config.json，给各模块提供启动配置。
 *
 * @details
 * M2 阶段只有 uart/ 消费 serial 字段；mqtt/logging 字段现在就解析出来放进
 * AppConfig（对应 config.example.json 的完整 schema），但暂时没人用——
 * 等 M5（MQTT）、日志接入这些模块实现时再从这里取，不需要再改这个文件的 schema。
 * 依赖边界：只依赖 nlohmann/json，不包含 uart/、mqtt/ 等其他模块头文件。
 */

#include <expected>
#include <filesystem>
#include <string>

namespace config {

/// 配置加载失败的原因。
enum class ConfigError {
  kFileNotFound,   ///< 文件路径打不开
  kParseError,     ///< 内容不是合法 JSON
  kMissingField,   ///< 缺少必需字段
  kInvalidValue,   ///< 字段存在但类型不对
};

struct SerialConfig {
  std::string device;  ///< 字符设备路径，例如 "/dev/ttyUSB0"
  int baud = 0;        ///< 波特率
};

struct MqttConfig {
  std::string broker_host;
  int broker_port = 0;
  std::string client_id;
  std::string username;
  std::string password;
  std::string topic_prefix;
  int qos = 0;
  int keepalive_seconds = 0;
};

struct LoggingConfig {
  std::string level;
  std::string file;
};

struct AppConfig {
  SerialConfig serial;
  MqttConfig mqtt;
  LoggingConfig logging;
};

/**
 * @brief 读取并解析 path 指向的 JSON 配置文件。
 * @return 成功返回填好的 AppConfig；失败返回具体错误原因（不抛异常）。
 */
std::expected<AppConfig, ConfigError> LoadAppConfig(const std::filesystem::path& path);

}  // namespace config
```

- [ ] **Step 4: 写 `src/config/app_config.cpp`**

```cpp
/**
 * @file app_config.cpp
 * @brief app_config.hpp 的实现。
 */

#include "config/app_config.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

namespace config {

std::expected<AppConfig, ConfigError> LoadAppConfig(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    return std::unexpected(ConfigError::kFileNotFound);
  }

  nlohmann::json root;
  try {
    in >> root;
  } catch (const nlohmann::json::parse_error&) {
    return std::unexpected(ConfigError::kParseError);
  }

  AppConfig cfg;
  try {
    const auto& serial = root.at("serial");
    cfg.serial.device = serial.at("device").get<std::string>();
    cfg.serial.baud = serial.at("baud").get<int>();

    const auto& mqtt = root.at("mqtt");
    cfg.mqtt.broker_host = mqtt.at("broker_host").get<std::string>();
    cfg.mqtt.broker_port = mqtt.at("broker_port").get<int>();
    cfg.mqtt.client_id = mqtt.at("client_id").get<std::string>();
    cfg.mqtt.username = mqtt.at("username").get<std::string>();
    cfg.mqtt.password = mqtt.at("password").get<std::string>();
    cfg.mqtt.topic_prefix = mqtt.at("topic_prefix").get<std::string>();
    cfg.mqtt.qos = mqtt.at("qos").get<int>();
    cfg.mqtt.keepalive_seconds = mqtt.at("keepalive_seconds").get<int>();

    const auto& logging = root.at("logging");
    cfg.logging.level = logging.at("level").get<std::string>();
    cfg.logging.file = logging.at("file").get<std::string>();
  } catch (const nlohmann::json::out_of_range&) {
    return std::unexpected(ConfigError::kMissingField);
  } catch (const nlohmann::json::type_error&) {
    return std::unexpected(ConfigError::kInvalidValue);
  }

  return cfg;
}

}  // namespace config
```

- [ ] **Step 5: 把新源文件加进 `cns_rpi_core` 库**

编辑 `CMakeLists.txt`，把：

```cmake
add_library(cns_rpi_core
    src/uart/serial_port.cpp
    src/uart/mavlink_link.cpp
)
```

改成：

```cmake
add_library(cns_rpi_core
    src/uart/serial_port.cpp
    src/uart/mavlink_link.cpp
    src/config/app_config.cpp
)
```

- [ ] **Step 6: 运行测试，确认全部通过**

Run: `cmake -B build -S . && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: 编译零警告；`app_config` 和 `mavlink_link` 两个 test 都通过。

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/config/app_config.hpp src/config/app_config.cpp tests/test_app_config.cpp
git commit -m "$(cat <<'EOF'
feat: 新增JSON配置文件加载(app_config)
EOF
)"
```

---

### Task 5: `main.cpp` —— 双向收发最小闭环

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `config::LoadAppConfig`（Task 4）、`uart::MavlinkLink`（Task 3）、`common/mavlink.h` 里的 `mavlink_msg_heartbeat_pack`/`MAV_TYPE_ONBOARD_CONTROLLER`/`MAV_AUTOPILOT_INVALID`/`MAV_STATE_ACTIVE`/`MAV_COMP_ID_ONBOARD_COMPUTER`。

这一步是集成，不是新的可单测单元（真正的行为验证在 Task 8 用真实硬件跑）。只要求编译通过、逻辑走查正确。

- [ ] **Step 1: 改写 `src/main.cpp`**

```cpp
/**
 * @file main.cpp
 * @brief 程序入口，组合根。
 *
 * @details
 * M2 阶段接入 UART/MAVLink 收发帧层的最小闭环：
 * 读配置 -> 打开 mavlink_link -> 循环收帧打日志 + 周期发送本机 HEARTBEAT。
 * 不解析帧内容（M3 的事），不接 MQTT（M5 的事）。
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "common/mavlink.h"
#include "config/app_config.hpp"
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

  std::cout << "cns_rpi M2 启动，串口=" << app_config->serial.device
            << " 波特率=" << app_config->serial.baud << std::endl;

  auto last_heartbeat = std::chrono::steady_clock::now();
  while (true) {
    if (auto msg = link->ReceiveMessage()) {
      std::cout << "收到帧 msgid=" << static_cast<int>(msg->msgid)
                << " len=" << static_cast<int>(msg->len)
                << " sysid=" << static_cast<int>(msg->sysid) << std::endl;
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

- [ ] **Step 2: 验证编译零警告**

Run: `cmake -B build -S . && cmake --build build 2>&1 | tee /tmp/build.log && grep -i warning /tmp/build.log`
Expected: 构建成功，无警告输出。

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
feat: main.cpp接入UART/MAVLink双向收发最小闭环
EOF
)"
```

---

### Task 6: `scripts/install_deps.sh` —— 补充新依赖包

**Files:**
- Modify: `scripts/install_deps.sh`

- [ ] **Step 1: 在依赖安装步骤里加上新包**

把这一行（`step 2` 里）：

```bash
sudo apt install -y build-essential cmake git
```

改成：

```bash
sudo apt install -y build-essential cmake git nlohmann-json3-dev doctest-dev
```

同时把 `BANNER` 里对应这一步的描述从：

```
  [2] 安装构建依赖 -> build-essential cmake git
```

改成：

```
  [2] 安装构建依赖 -> build-essential cmake git nlohmann-json3-dev doctest-dev
      （nlohmann-json3-dev 是配置文件解析用的头文件库；doctest-dev 是单元测试框架，只在开发机/CI需要，
      不影响 systemd 部署的运行时依赖）
```

- [ ] **Step 2: 语法检查脚本没写错**

Run: `bash -n scripts/install_deps.sh`
Expected: 无输出（没有语法错误）。

- [ ] **Step 3: Commit**

```bash
git add scripts/install_deps.sh
git commit -m "$(cat <<'EOF'
build: install_deps.sh补充nlohmann-json3-dev和doctest-dev
EOF
)"
```

---

### Task 7: 文档同步 —— V1设计文档.md 标记 UART 分配已解决

**Files:**
- Modify: `docs/V1设计文档.md`

`docs/协作规则.md` §7 要求架构/协议对接范围变化要同步更新设计文档；M2 落地后，§10/§11 里"UART 口号/波特率待固件分配"这个阻塞项已经在 `docs/superpowers/specs/2026-07-02-m2-uart-mavlink-link-design.md` 里解决了（UART1，经开发板板载桥接芯片，115200 baud），这一步把设计文档的状态改到和实现同步。

- [ ] **Step 1: 更新 §10 M2 里程碑那一条**

把：

```
- **M2 MAVLink 收发帧层**：vendor 官方头文件，用 `mavlink_frame_char_buffer()` 跑通串口原始字节 ↔ 完整 MAVLink 帧（先验证帧完整性和 CRC，双向都要跑通，不管内容）
  **阻塞项：需固件侧先分配新 UART（口号/波特率，TX+RX 都要配置）并能收发真实 HEARTBEAT**
```

改成：

```
- **M2 MAVLink 收发帧层**：vendor 官方头文件，用 `mavlink_frame_char_buffer()` 跑通串口原始字节 ↔ 完整 MAVLink 帧（先验证帧完整性和 CRC，双向都要跑通，不管内容）——已实现，见 `docs/superpowers/plans/2026-07-03-m2-uart-mavlink-link.md`
  ~~阻塞项：需固件侧先分配新 UART（口号/波特率，TX+RX 都要配置）并能收发真实 HEARTBEAT~~ ——已解决：UART1，经开发板板载桥接芯片（miniUSB口，跳线帽选通），115200 baud，物理链路 TX/RX 双向已在真机验证；固件侧真正从 UART1 发出 HEARTBEAT 仍待固件实现，不再阻塞 RPi 侧代码
```

- [ ] **Step 2: 更新 §11 开放问题里对应条目**

把：

```
- STM32 侧新 UART 的口号、波特率，且需确认 RX 方向（命令下行）也会配置——阻塞 M2
```

改成：

```
- ~~STM32 侧新 UART 的口号、波特率，且需确认 RX 方向（命令下行）也会配置~~ ——已解决：UART1（经开发板板载 CH340/CH341 桥接芯片，miniUSB 口跳线选通），115200 baud，详见 `docs/superpowers/specs/2026-07-02-m2-uart-mavlink-link-design.md` §1，不再阻塞 M2
```

- [ ] **Step 3: 检查改动只涉及这两处，没有引入其他改动**

Run: `git diff docs/V1设计文档.md`
Expected: diff 只包含上面两处替换。

- [ ] **Step 4: Commit**

```bash
git add docs/V1设计文档.md
git commit -m "$(cat <<'EOF'
docs: M2落地后标记UART口号/波特率开放问题已解决
EOF
)"
```

---

### Task 8: 物理链路人工验证（不进 CI，手动跑一遍）

**Files:** 无代码改动；本任务只产出验证结果，写进本计划文档末尾或下一次提交的 commit message 里说明"验证过什么、用的什么链路"。

前置：真实树莓派 `dcdw@192.168.11.4` 可 SSH 访问；`tools/mavlink_sim/` 已具备（README 见该目录）；开发机没有 miniUSB 口，所以这一步用"两颗 CH340 + 杜邦线桥接"的临时链路（开发机侧一颗、RPi 侧一颗），不是最终的板载 miniUSB 直连——这对 RPi 这端的验证结论没有影响，两种物理链路在 RPi 上都表现为同一类 `ch341-uart` 驱动的 `/dev/ttyUSBx` 设备。

- [ ] **Step 1: 把新代码同步到真实 RPi 并编译**

```bash
ssh dcdw@192.168.11.4 "cd ~/cns_rpi && git pull && cmake -B build -S . && cmake --build build"
```

Expected: 编译成功，生成 `build/cns_rpi`。

- [ ] **Step 2: 在 RPi 上准备真实配置文件**

```bash
ssh dcdw@192.168.11.4 "cd ~/cns_rpi && cp config/config.example.json config/config.json"
```

用 `ssh dcdw@192.168.11.4 "ls /dev/ttyUSB*"` 确认当前 RPi 侧 CH340 设备路径和 `config/config.json` 里的 `serial.device` 一致（如果不是 `/dev/ttyUSB0`，手动改一下这份 `config.json`，不要改 `config.example.json`）。

- [ ] **Step 3: 开发机上装 pymavlink 模拟工具（如果之前没装过）**

```bash
python3 -m venv ~/.venvs/mavlink-sim
~/.venvs/mavlink-sim/bin/pip install pymavlink pyserial
```

- [ ] **Step 4: 验证接收方向——开发机模拟 STM32 发送，RPi 上跑真实 C++ 接收**

在 RPi 上（一个 SSH 会话）：

```bash
ssh dcdw@192.168.11.4 "cd ~/cns_rpi && ./build/cns_rpi config/config.json"
```

在开发机上（另一个终端，开发机侧 CH340 设备路径按实际 `lsusb`/`ls /dev/ttyUSB*` 结果填）：

```bash
~/.venvs/mavlink-sim/bin/python3 tools/mavlink_sim/send_frames.py --port /dev/ttyUSB0 --baud 115200
```

Expected: RPi 终端持续打印 `收到帧 msgid=... len=... sysid=...`，`msgid` 应该能看到 `send_frames.py` 发送的几种消息（HEARTBEAT=0、GPS_RAW_INT=24、ATTITUDE=30、SYS_STATUS=1、NAMED_VALUE_INT=252）循环出现。

- [ ] **Step 5: 验证发送方向——RPi 上真实 C++ 发送的 HEARTBEAT 能被开发机正确解码**

保持 `cns_rpi` 继续跑着，在开发机上另开一个终端：

```bash
~/.venvs/mavlink-sim/bin/python3 tools/mavlink_sim/receive_frames.py --port /dev/ttyUSB0 --baud 115200
```

Expected: 大约每 1 秒能看到一条被正确解码的 `HEARTBEAT`（`type`=`MAV_TYPE_ONBOARD_CONTROLLER`=18，`autopilot`=`MAV_AUTOPILOT_INVALID`=8）。

- [ ] **Step 6: 停止两端进程，记录结果**

`Ctrl+C` 停掉 RPi 上的 `cns_rpi` 和开发机上的两个 Python 脚本。如果 Step 4/5 都符合预期，把这次验证记一笔到下一次相关提交的 commit message 里（或者如果后续有独立的变更记录文档，按 `docs/协作规则.md` §4 补一份），说明"物理链路验证：临时 CH340 桥接，双向收发正常；miniUSB 直连链路留待固件侧 UART1 真正发送 HEARTBEAT 后复核"——这样任何人看提交历史都知道这条链路测过什么、没测过什么。
