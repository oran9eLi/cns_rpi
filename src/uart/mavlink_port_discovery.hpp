#pragma once

#include <chrono>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <thread>

#include "uart/mavlink_link.hpp"

namespace uart {

/// 限制等待发现串口时的重复日志；首次立即放行，之后按间隔放行。
class DiscoveryLogLimiter {
 public:
  using Clock = std::chrono::steady_clock;

  explicit DiscoveryLogLimiter(Clock::duration interval)
      : interval_(interval) {}

  bool ShouldLog(Clock::time_point now);

 private:
  Clock::duration interval_;
  std::optional<Clock::time_point> last_log_;
};

/// 从首个合法 MAVLink 帧开始监测静默时长。
class MavlinkSilenceWatchdog {
 public:
  using Clock = std::chrono::steady_clock;

  explicit MavlinkSilenceWatchdog(Clock::duration timeout)
      : timeout_(timeout) {}

  void ObserveValidFrame(Clock::time_point now) { last_frame_ = now; }
  bool Expired(Clock::time_point now) const;
  void Reset() { last_frame_.reset(); }

 private:
  Clock::duration timeout_;
  std::optional<Clock::time_point> last_frame_;
};

std::vector<std::string> OrderSerialCandidates(
    std::span<const std::string> candidates);

std::vector<std::string> EnumerateSerialCandidates();

/// 候选串口打开或读取失败的设备路径与明确原因。
struct CandidateFailure {
  std::string device;
  UartError error;
};

/// 已通过完整合法 MAVLink 帧确认的串口，并保留确认时收到的首帧供上层继续处理。
struct DiscoveredMavlinkPort {
  std::string device;
  MavlinkLink link;
  mavlink_message_t first_message{};
};

/// 单轮发现结果；未找到时 found 为空，failures 保留可诊断的串口故障。
struct DiscoveryAttempt {
  std::optional<DiscoveredMavlinkPort> found;
  std::vector<CandidateFailure> failures;
};

/// 将候选失败压缩为适合主日志的一行中文诊断。
std::string FormatCandidateFailures(
    std::span<const CandidateFailure> failures);

using StopRequested = std::function<bool()>;

/// 按给定顺序逐个监听候选串口，首个收到 CRC 正确完整帧的候选即确认成功。
DiscoveryAttempt ProbeMavlinkCandidates(
    std::span<const std::string> candidates, int baud,
    std::chrono::milliseconds per_port_timeout,
    const StopRequested& stop_requested);

/// 执行一轮发现；auto 枚举系统候选，明确路径则只探测该路径。
DiscoveryAttempt DiscoverMavlinkPortOnce(
    std::string_view configured_device, int baud,
    std::chrono::milliseconds per_port_timeout,
    const StopRequested& stop_requested);

/// 在后台执行一轮串口发现，避免逐口探测阻塞主事件循环。
class AsyncMavlinkDiscovery {
 public:
  AsyncMavlinkDiscovery() = default;
  AsyncMavlinkDiscovery(const AsyncMavlinkDiscovery&) = delete;
  AsyncMavlinkDiscovery& operator=(const AsyncMavlinkDiscovery&) = delete;

  bool Start(std::string configured_device, int baud,
             std::chrono::milliseconds per_port_timeout);
  bool IsRunning() const { return running_.load(std::memory_order_acquire); }
  std::optional<DiscoveryAttempt> TryTakeResult();

 private:
  std::mutex mutex_;
  std::optional<DiscoveryAttempt> result_;
  std::atomic<bool> running_{false};
  std::jthread worker_;
};

}  // 命名空间 uart
