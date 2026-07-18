#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "uart/mavlink_link.hpp"

namespace uart {

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

}  // 命名空间 uart
