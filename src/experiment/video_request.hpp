/**
 * @file video_request.hpp
 * @brief 视频实验的外部 JSON 入口；通过后仅向执行层传递类型化值。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "video/video_manager.hpp"

namespace experiment {

struct VideoRequest {
  std::string command_id, device_id, session_id, action_id, operation;
  std::uint64_t lease_version{0};
  std::chrono::system_clock::time_point expiry;
  video::Request video;
  std::string comparison;
};

bool IsVideoOperation(std::string_view payload);
std::expected<VideoRequest, std::string> ParseVideoRequest(std::string_view payload);

}  // namespace experiment
