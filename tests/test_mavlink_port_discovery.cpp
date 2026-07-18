#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "uart/mavlink_port_discovery.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "common/mavlink.h"

namespace {

struct PtyPair {
  int master_fd{-1};
  std::string slave_path;

  PtyPair() = default;
  PtyPair(const PtyPair&) = delete;
  PtyPair& operator=(const PtyPair&) = delete;
  PtyPair(PtyPair&& other) noexcept
      : master_fd(std::exchange(other.master_fd, -1)),
        slave_path(std::move(other.slave_path)) {}
  PtyPair& operator=(PtyPair&&) = delete;
  ~PtyPair() {
    if (master_fd >= 0) {
      ::close(master_fd);
    }
  }
};

PtyPair OpenPtyPair() {
  PtyPair pty;
  pty.master_fd = ::posix_openpt(O_RDWR | O_NOCTTY);
  REQUIRE(pty.master_fd >= 0);
  REQUIRE(::grantpt(pty.master_fd) == 0);
  REQUIRE(::unlockpt(pty.master_fd) == 0);
  const char* slave_name = ::ptsname(pty.master_fd);
  REQUIRE(slave_name != nullptr);
  pty.slave_path = slave_name;
  {
    auto raw_port = uart::SerialPort::Open(pty.slave_path, 115200);
    REQUIRE(raw_port.has_value());
  }
  return pty;
}

void WriteAll(int fd, const std::vector<std::uint8_t>& bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const auto count = ::write(fd, bytes.data() + written, bytes.size() - written);
    REQUIRE(count > 0);
    written += static_cast<std::size_t>(count);
  }
}

template <std::size_t Size>
void WriteAll(int fd, const std::array<std::uint8_t, Size>& bytes) {
  WriteAll(fd, std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

std::vector<std::uint8_t> Encode(const mavlink_message_t& message) {
  std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> buffer{};
  const auto size = mavlink_msg_to_send_buffer(buffer.data(), &message);
  return {buffer.begin(), buffer.begin() + size};
}

mavlink_message_t PackHeartbeat(std::uint8_t system_id) {
  mavlink_message_t message{};
  mavlink_msg_heartbeat_pack(system_id, MAV_COMP_ID_AUTOPILOT1, &message,
                             MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_GENERIC,
                             0, 0, MAV_STATE_ACTIVE);
  return message;
}

mavlink_message_t PackSystemTime(std::uint8_t system_id) {
  mavlink_message_t message{};
  mavlink_msg_system_time_pack(system_id, MAV_COMP_ID_AUTOPILOT1, &message,
                               123456U, 789U);
  return message;
}

}  // 匿名命名空间

TEST_CASE("串口等待告警十秒内只允许一次") {
  uart::DiscoveryLogLimiter limiter(std::chrono::seconds(10));
  const auto start = uart::DiscoveryLogLimiter::Clock::time_point{};

  CHECK(limiter.ShouldLog(start));
  CHECK_FALSE(limiter.ShouldLog(start + std::chrono::seconds(9)));
  CHECK(limiter.ShouldLog(start + std::chrono::seconds(10)));
}

TEST_CASE("MAVLink静默达到十秒才判定失联") {
  uart::MavlinkSilenceWatchdog watchdog(std::chrono::seconds(10));
  const auto start = uart::MavlinkSilenceWatchdog::Clock::time_point{};

  watchdog.ObserveValidFrame(start);
  CHECK_FALSE(watchdog.Expired(start + std::chrono::seconds(9)));
  CHECK(watchdog.Expired(start + std::chrono::seconds(10)));
  watchdog.ObserveValidFrame(start + std::chrono::seconds(11));
  CHECK_FALSE(watchdog.Expired(start + std::chrono::seconds(20)));
}

TEST_CASE("MAVLink静默看门狗未见首帧和重置后不误报") {
  uart::MavlinkSilenceWatchdog watchdog(std::chrono::seconds(10));
  const auto start = uart::MavlinkSilenceWatchdog::Clock::time_point{};

  CHECK_FALSE(watchdog.Expired(start + std::chrono::hours(1)));
  watchdog.ObserveValidFrame(start);
  REQUIRE(watchdog.Expired(start + std::chrono::seconds(10)));
  watchdog.Reset();
  CHECK_FALSE(watchdog.Expired(start + std::chrono::hours(1)));
}

TEST_CASE("USB候选按数值自然排序后从两端交替") {
  const std::vector<std::string> input{
      "/dev/ttyUSB10", "/dev/ttyUSB2", "/dev/ttyUSB1",
      "/dev/ttyUSB3",  "/dev/ttyACM2", "/dev/ttyACM0"};
  CHECK(uart::OrderSerialCandidates(input) ==
        std::vector<std::string>{
            "/dev/ttyUSB1", "/dev/ttyUSB10", "/dev/ttyUSB2",
            "/dev/ttyUSB3", "/dev/ttyACM0",  "/dev/ttyACM2"});
}

TEST_CASE("候选排序去重且奇数数量不遗漏中间项") {
  const std::vector<std::string> input{
      "/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB1",
      "/dev/ttyUSB2", "/dev/ttyUSB3", "/dev/ttyUSB4"};
  CHECK(uart::OrderSerialCandidates(input) ==
        std::vector<std::string>{
            "/dev/ttyUSB0", "/dev/ttyUSB4", "/dev/ttyUSB1",
            "/dev/ttyUSB3", "/dev/ttyUSB2"});
}

TEST_CASE("垃圾端口被跳过且合法MAVLink首帧被保留") {
  auto garbage = OpenPtyPair();
  auto mavlink = OpenPtyPair();
  REQUIRE(garbage.slave_path != mavlink.slave_path);
  const std::array<std::uint8_t, 4> garbage_bytes{'A', 'T', '\r', '\n'};
  WriteAll(garbage.master_fd, garbage_bytes);
  WriteAll(mavlink.master_fd, Encode(PackHeartbeat(17)));

  const std::vector<std::string> candidates{garbage.slave_path,
                                             mavlink.slave_path};
  auto attempt = uart::ProbeMavlinkCandidates(
      candidates, 115200, std::chrono::milliseconds(250),
      [] { return false; });

  REQUIRE(attempt.found.has_value());
  CHECK(attempt.found->device == mavlink.slave_path);
  CHECK(attempt.found->first_message.sysid == 17);
  CHECK(static_cast<std::uint32_t>(attempt.found->first_message.msgid) ==
        MAVLINK_MSG_ID_HEARTBEAT);
}

TEST_CASE("CRC损坏的完整帧不能确认端口") {
  auto pty = OpenPtyPair();
  auto bytes = Encode(PackHeartbeat(18));
  bytes[10] ^= 0xFF;
  WriteAll(pty.master_fd, bytes);

  const std::vector<std::string> candidates{pty.slave_path};
  auto attempt = uart::ProbeMavlinkCandidates(
      candidates, 115200, std::chrono::milliseconds(150),
      [] { return false; });

  CHECK_FALSE(attempt.found.has_value());
}

TEST_CASE("非HEARTBEAT合法帧也能确认端口") {
  auto pty = OpenPtyPair();
  WriteAll(pty.master_fd, Encode(PackSystemTime(19)));

  const std::vector<std::string> candidates{pty.slave_path};
  auto attempt = uart::ProbeMavlinkCandidates(
      candidates, 115200, std::chrono::milliseconds(250),
      [] { return false; });

  REQUIRE(attempt.found.has_value());
  CHECK(attempt.found->first_message.sysid == 19);
  CHECK(static_cast<std::uint32_t>(attempt.found->first_message.msgid) ==
        MAVLINK_MSG_ID_SYSTEM_TIME);
}

TEST_CASE("停止回调及时中止本轮探测") {
  auto first = OpenPtyPair();
  auto second = OpenPtyPair();
  const std::vector<std::string> candidates{first.slave_path, second.slave_path};
  int checks = 0;

  const auto start = std::chrono::steady_clock::now();
  auto attempt = uart::ProbeMavlinkCandidates(
      candidates, 115200, std::chrono::seconds(2),
      [&checks] { return ++checks >= 2; });
  const auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK_FALSE(attempt.found.has_value());
  CHECK(checks >= 2);
  CHECK(elapsed < std::chrono::milliseconds(500));
}

TEST_CASE("等待串口期间出现停止请求会及时中止") {
  auto pty = OpenPtyPair();
  const std::vector<std::string> candidates{pty.slave_path};
  std::atomic<bool> stopped{false};
  std::jthread requester([&stopped] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stopped.store(true);
  });

  const auto start = std::chrono::steady_clock::now();
  auto attempt = uart::ProbeMavlinkCandidates(
      candidates, 115200, std::chrono::seconds(3),
      [&stopped] { return stopped.load(); });
  const auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK_FALSE(attempt.found.has_value());
  CHECK(elapsed < std::chrono::milliseconds(250));
}

TEST_CASE("短超时不会为每个候选阻塞一百毫秒") {
  auto first = OpenPtyPair();
  auto second = OpenPtyPair();
  auto third = OpenPtyPair();
  const std::vector<std::string> candidates{
      first.slave_path, second.slave_path, third.slave_path};

  const auto start = std::chrono::steady_clock::now();
  auto attempt = uart::ProbeMavlinkCandidates(
      candidates, 115200, std::chrono::milliseconds(1),
      [] { return false; });
  const auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK_FALSE(attempt.found.has_value());
  CHECK(elapsed < std::chrono::milliseconds(50));
}

TEST_CASE("首个设备忙时记录失败原因并继续探测") {
  auto busy = OpenPtyPair();
  auto held = uart::MavlinkLink::Open(busy.slave_path, 115200);
  REQUIRE(held.has_value());
  auto mavlink = OpenPtyPair();
  WriteAll(mavlink.master_fd, Encode(PackHeartbeat(20)));

  const std::vector<std::string> candidates{busy.slave_path, mavlink.slave_path};
  auto attempt = uart::ProbeMavlinkCandidates(
      candidates, 115200, std::chrono::milliseconds(250),
      [] { return false; });

  REQUIRE(attempt.found.has_value());
  REQUIRE(attempt.failures.size() == 1);
  CHECK(attempt.failures.front().device == busy.slave_path);
  CHECK(attempt.failures.front().error == uart::UartError::kDeviceBusy);
}

TEST_CASE("明确路径只探测配置的设备") {
  auto configured = OpenPtyPair();
  auto other = OpenPtyPair();
  WriteAll(other.master_fd, Encode(PackHeartbeat(21)));

  auto attempt = uart::DiscoverMavlinkPortOnce(
      configured.slave_path, 115200, std::chrono::milliseconds(150),
      [] { return false; });

  CHECK_FALSE(attempt.found.has_value());
  CHECK(attempt.failures.empty());
}

TEST_CASE("后台发现启动后不会阻塞主循环") {
  auto silent = OpenPtyPair();
  uart::AsyncMavlinkDiscovery discovery;

  const auto start = std::chrono::steady_clock::now();
  REQUIRE(discovery.Start(silent.slave_path, 115200,
                          std::chrono::milliseconds(300)));
  const auto start_elapsed = std::chrono::steady_clock::now() - start;

  CHECK(start_elapsed < std::chrono::milliseconds(50));
  CHECK(discovery.IsRunning());
  CHECK_FALSE(discovery.TryTakeResult().has_value());

  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  auto result = discovery.TryTakeResult();
  REQUIRE(result.has_value());
  CHECK_FALSE(result->found.has_value());
  CHECK_FALSE(discovery.IsRunning());
}

TEST_CASE("后台发现完成后返回首个合法帧") {
  auto mavlink = OpenPtyPair();
  WriteAll(mavlink.master_fd, Encode(PackHeartbeat(22)));
  uart::AsyncMavlinkDiscovery discovery;

  REQUIRE(discovery.Start(mavlink.slave_path, 115200,
                          std::chrono::milliseconds(300)));
  std::optional<uart::DiscoveryAttempt> result;
  for (int i = 0; i < 20 && !result; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    result = discovery.TryTakeResult();
  }

  REQUIRE(result.has_value());
  REQUIRE(result->found.has_value());
  CHECK(result->found->device == mavlink.slave_path);
  CHECK(result->found->first_message.sysid == 22);
}

TEST_CASE("串口发现失败诊断包含设备和中文原因") {
  const std::vector<uart::CandidateFailure> failures{
      {"/dev/ttyUSB1", uart::UartError::kPermissionDenied},
      {"/dev/ttyUSB2", uart::UartError::kDeviceBusy},
  };

  CHECK(uart::FormatCandidateFailures(failures) ==
        "/dev/ttyUSB1=权限不足, /dev/ttyUSB2=设备忙");
}
