#include "latency/px4_latency.hpp"

#include <algorithm>
#include <cctype>

namespace latency {
namespace {

constexpr std::size_t kMinCorrelationIdLength = 8;
constexpr std::size_t kMaxCorrelationIdLength = 128;

bool IsValidCorrelationId(const std::string& value) {
  if (value.size() < kMinCorrelationIdLength ||
      value.size() > kMaxCorrelationIdLength) {
    return false;
  }
  return std::ranges::all_of(value, [](unsigned char character) {
    return std::isalnum(character) != 0 || character == '.' ||
           character == '_' || character == ':' || character == '-';
  });
}

}  // namespace

std::expected<Px4LatencyProbe, std::string> ParsePx4LatencyProbe(
    std::string_view payload, std::string_view expected_device_id) {
  const auto document =
      nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return std::unexpected("payload不是JSON对象");
  }
  if (document.value("schema_version", 0) != 1 ||
      !document.contains("device_id") ||
      !document["device_id"].is_string() ||
      !document.contains("session_id") ||
      !document["session_id"].is_string() ||
      !document.contains("probe_id") ||
      !document["probe_id"].is_string()) {
    return std::unexpected("payload字段不符合v1协议");
  }

  Px4LatencyProbe probe{
      .device_id = document["device_id"].get<std::string>(),
      .session_id = document["session_id"].get<std::string>(),
      .probe_id = document["probe_id"].get<std::string>(),
  };
  if (probe.device_id != expected_device_id) {
    return std::unexpected("device_id与当前设备不一致");
  }
  if (!IsValidCorrelationId(probe.session_id) ||
      !IsValidCorrelationId(probe.probe_id)) {
    return std::unexpected("关联标识符格式非法");
  }
  return probe;
}

nlohmann::json BuildPx4LatencyAck(const Px4LatencyProbe& probe) {
  return {
      {"schema_version", 1},
      {"device_id", probe.device_id},
      {"session_id", probe.session_id},
      {"probe_id", probe.probe_id},
  };
}

}  // namespace latency
