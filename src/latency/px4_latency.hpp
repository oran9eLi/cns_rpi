#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace latency {

struct Px4LatencyProbe {
  std::string device_id;
  std::string session_id;
  std::string probe_id;
};

std::expected<Px4LatencyProbe, std::string> ParsePx4LatencyProbe(
    std::string_view payload, std::string_view expected_device_id);

nlohmann::json BuildPx4LatencyAck(const Px4LatencyProbe& probe);

}  // namespace latency
