#include "uart/mavlink_port_discovery.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace uart {
namespace {

/// 单次等待不超过该片长，保证串口静默时也能及时响应停止请求。
constexpr auto kStopPollingInterval = std::chrono::milliseconds(50);

struct NumberedPath {
  std::string path;
  std::uint64_t number;
};

std::optional<NumberedPath> ParseNumberedPath(const std::string& path,
                                              std::string_view prefix) {
  if (!path.starts_with(prefix)) {
    return std::nullopt;
  }

  const std::string_view suffix(path.data() + prefix.size(),
                                path.size() - prefix.size());
  if (suffix.empty()) {
    return std::nullopt;
  }

  std::uint64_t number = 0;
  const auto [end, error] =
      std::from_chars(suffix.data(), suffix.data() + suffix.size(), number);
  if (error != std::errc{} || end != suffix.data() + suffix.size()) {
    return std::nullopt;
  }
  return NumberedPath{path, number};
}

std::vector<std::string> AlternateEnds(std::vector<NumberedPath> paths) {
  std::ranges::sort(paths, {}, &NumberedPath::number);
  std::vector<std::string> out;
  out.reserve(paths.size());
  std::size_t low = 0;
  std::size_t high = paths.size();
  while (low < high) {
    out.push_back(std::move(paths[low++].path));
    if (low < high) {
      out.push_back(std::move(paths[--high].path));
    }
  }
  return out;
}

}  // 匿名命名空间

bool DiscoveryLogLimiter::ShouldLog(Clock::time_point now) {
  if (!last_log_ || now - *last_log_ >= interval_) {
    last_log_ = now;
    return true;
  }
  return false;
}

bool MavlinkSilenceWatchdog::Expired(Clock::time_point now) const {
  return last_frame_.has_value() && now - *last_frame_ >= timeout_;
}

std::vector<std::string> OrderSerialCandidates(
    std::span<const std::string> candidates) {
  constexpr std::string_view kUsbPrefix = "/dev/ttyUSB";
  constexpr std::string_view kAcmPrefix = "/dev/ttyACM";
  std::vector<NumberedPath> usb;
  std::vector<NumberedPath> acm;
  std::unordered_set<std::string> seen;

  for (const auto& candidate : candidates) {
    if (!seen.insert(candidate).second) {
      continue;
    }
    if (auto path = ParseNumberedPath(candidate, kUsbPrefix)) {
      usb.push_back(std::move(*path));
    } else if (auto path = ParseNumberedPath(candidate, kAcmPrefix)) {
      acm.push_back(std::move(*path));
    }
  }

  auto ordered = AlternateEnds(std::move(usb));
  auto ordered_acm = AlternateEnds(std::move(acm));
  ordered.reserve(ordered.size() + ordered_acm.size());
  std::ranges::move(ordered_acm, std::back_inserter(ordered));
  return ordered;
}

std::vector<std::string> EnumerateSerialCandidates() {
  std::vector<std::string> candidates;
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      "/dev", std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    std::error_code type_error;
    if (iterator->is_character_file(type_error) && !type_error) {
      candidates.push_back(iterator->path().string());
    }
    iterator.increment(error);
  }
  return OrderSerialCandidates(candidates);
}

DiscoveryAttempt ProbeMavlinkCandidates(
    std::span<const std::string> candidates, int baud,
    std::chrono::milliseconds per_port_timeout,
    const StopRequested& stop_requested) {
  DiscoveryAttempt attempt;
  for (const auto& device : candidates) {
    if (stop_requested()) {
      break;
    }

    auto link = MavlinkLink::Open(device, baud);
    if (!link) {
      attempt.failures.push_back({device, link.error()});
      continue;
    }

    const auto deadline = std::chrono::steady_clock::now() + per_port_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (stop_requested()) {
        return attempt;
      }

      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      auto received =
          link->ReceiveMessage(std::min(remaining, kStopPollingInterval));
      if (!received) {
        attempt.failures.push_back({device, received.error()});
        break;
      }
      if (received->has_value()) {
        attempt.found.emplace(
            DiscoveredMavlinkPort{device, std::move(*link), **received});
        return attempt;
      }
    }
  }
  return attempt;
}

DiscoveryAttempt DiscoverMavlinkPortOnce(
    std::string_view configured_device, int baud,
    std::chrono::milliseconds per_port_timeout,
    const StopRequested& stop_requested) {
  if (configured_device == "auto") {
    const auto candidates = EnumerateSerialCandidates();
    return ProbeMavlinkCandidates(candidates, baud, per_port_timeout,
                                  stop_requested);
  }

  const std::array<std::string, 1> candidates{
      std::string(configured_device)};
  return ProbeMavlinkCandidates(candidates, baud, per_port_timeout,
                                stop_requested);
}

}  // 命名空间 uart
