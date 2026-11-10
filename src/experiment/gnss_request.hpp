/** @file gnss_request.hpp @brief GNSS实验动作的JSON入口，内部仅消费已校验值。 */
#pragma once
#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace experiment {
struct GnssRequest {
  std::string command_id, device_id, session_id, action_id, operation;
  std::uint64_t lease_version{0};
  std::chrono::system_clock::time_point expiry;
  std::string capture_id;
  std::uint32_t duration_seconds{0};
  std::string comparison;
};
bool IsGnssOperation(std::string_view payload);
std::expected<GnssRequest, std::string> ParseGnssRequest(std::string_view payload);
}
