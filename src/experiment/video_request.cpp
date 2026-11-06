/**
 * @file video_request.cpp
 * @brief 视频操作的入口校验；保持已有实验身份、动作和过期时间语义。
 */
#include "experiment/video_request.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <ranges>

#include <nlohmann/json.hpp>

#include "protocol/identity.hpp"

namespace experiment {
namespace {
using Json = nlohmann::json;
constexpr std::array<std::string_view, 5> kOperations{
    "video_probe", "video_test", "video_start", "video_stop", "video_status"};

bool Uuid(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else if (!((value[i] >= '0' && value[i] <= '9') ||
                 (value[i] >= 'a' && value[i] <= 'f') ||
                 (value[i] >= 'A' && value[i] <= 'F'))) return false;
  }
  return true;
}

bool Keys(const Json& value, std::initializer_list<std::string_view> keys) {
  return value.is_object() && value.size() == keys.size() &&
         std::ranges::all_of(keys, [&](auto key) { return value.contains(key); });
}

std::optional<int> Integer(const Json& value, std::string_view key, int low, int high) {
  const auto& number = value.at(key);
  if (!number.is_number_integer()) return std::nullopt;
  if (number.is_number_unsigned()) {
    if (number.get<std::uint64_t>() > static_cast<std::uint64_t>(high)) return std::nullopt;
  } else if (number.get<std::int64_t>() < low || number.get<std::int64_t>() > high) {
    return std::nullopt;
  }
  const int result = number.get<int>();
  return result >= low ? std::optional{result} : std::nullopt;
}

std::optional<std::chrono::system_clock::time_point> Utc(std::string_view value) {
  if (value.size() != 24 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value[19] != '.' || value[23] != 'Z') return std::nullopt;
  const auto digits = [&](std::size_t offset, std::size_t count) -> std::optional<int> {
    int result = 0;
    const char* begin = value.data() + offset;
    if (!std::all_of(begin, begin + count, [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    const auto [end, ec] = std::from_chars(begin, begin + count, result);
    if (ec != std::errc{} || end != begin + count || result < 0) return std::nullopt;
    return result;
  };
  const auto y = digits(0, 4), m = digits(5, 2), d = digits(8, 2);
  const auto h = digits(11, 2), minute = digits(14, 2), s = digits(17, 2), ms = digits(20, 3);
  if (!y || !m || !d || !h || !minute || !s || !ms || *h > 23 || *minute > 59 || *s > 59) return std::nullopt;
  const std::chrono::year_month_day date{std::chrono::year{*y},
      std::chrono::month{static_cast<unsigned>(*m)}, std::chrono::day{static_cast<unsigned>(*d)}};
  if (!date.ok()) return std::nullopt;
  return std::chrono::sys_days{date} + std::chrono::hours{*h} + std::chrono::minutes{*minute} +
         std::chrono::seconds{*s} + std::chrono::milliseconds{*ms};
}

bool Alphanumeric(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool Host(std::string_view host) {
  if (host.empty() || host.size() > 253) return false;
  std::size_t start = 0;
  while (start < host.size()) {
    const auto end = host.find('.', start);
    const auto label = host.substr(start, end == host.npos ? host.size() - start : end - start);
    if (label.empty() || label.size() > 63 || !Alphanumeric(label.front()) ||
        !Alphanumeric(label.back()) || !std::ranges::all_of(label, [](char c) {
          return Alphanumeric(c) || c == '-';
        })) return false;
    if (end == host.npos) return true;
    start = end + 1;
  }
  return false;
}

bool StreamPath(std::string_view path) {
  return !path.empty() && path.size() <= 128 && path.front() != '/' && path.back() != '/' &&
         path.find("//") == path.npos && std::ranges::all_of(path, [](char c) {
           return Alphanumeric(c) || c == '_' || c == '-' || c == '/';
         });
}
}  // namespace

bool IsVideoOperation(std::string_view payload) {
  if (payload.size() > 4096) return false;
  const auto value = Json::parse(payload, nullptr, false);
  if (!value.is_object() || !value.contains("operation") || !value["operation"].is_string()) return false;
  return std::ranges::find(kOperations, value["operation"].get<std::string>()) != kOperations.end();
}

std::expected<VideoRequest, std::string> ParseVideoRequest(std::string_view payload) {
  if (payload.size() > 4096) return std::unexpected("视频动作载荷超过4096字节");
  try {
    auto root = Json::parse(payload);
    if (!root.is_object()) return std::unexpected("视频动作必须为对象");
    for (const auto key : {"command_id", "request_id", "session_id", "action_id", "operation", "expires_at"}) {
      if (!root.contains(key) || !root.at(key).is_string()) return std::unexpected("视频动作字段类型非法");
    }
    VideoRequest result;
    result.operation = root.at("operation").get<std::string>();
    if (std::ranges::find(kOperations, result.operation) == kOperations.end()) return std::unexpected("未知视频动作");
    const bool start = result.operation == "video_start";
    const bool test = result.operation == "video_test";
    const std::size_t expected_size = 9 + (start || test ? 1 : 0) + (start ? 1 : 0);
    constexpr std::array<std::string_view, 9> base_keys{"schema_version", "command_id", "request_id",
        "target", "session_id", "action_id", "lease_version", "operation", "expires_at"};
    if (root.size() != expected_size || !std::ranges::all_of(base_keys, [&](auto key) { return root.contains(key); }) ||
        !Integer(root, "schema_version", 1, 1) || !Keys(root.at("target"), {"device_id"}) ||
        !root.at("target").at("device_id").is_string()) return std::unexpected("视频动作字段非法");
    result.command_id = root.at("command_id").get<std::string>();
    result.device_id = root.at("target").at("device_id").get<std::string>();
    result.session_id = root.at("session_id").get<std::string>();
    result.action_id = root.at("action_id").get<std::string>();
    const auto& lease = root.at("lease_version");
    if (!Uuid(result.command_id) || !Uuid(result.session_id) || !Uuid(result.action_id) ||
        root.at("request_id").get<std::string>() != result.action_id || !protocol::IsValidUasId(result.device_id) ||
        !lease.is_number_integer() ||
        (!lease.is_number_unsigned() && lease.get<std::int64_t>() <= 0) ||
        (lease.is_number_unsigned() && lease.get<std::uint64_t>() == 0)) return std::unexpected("视频动作身份或租约非法");
    result.lease_version = lease.get<std::uint64_t>();
    const auto expiry = Utc(root.at("expires_at").get<std::string>());
    if (!expiry) return std::unexpected("视频动作过期时间非法");
    result.expiry = *expiry;
    if (start || test) {
      const auto& p = root.at("parameters");
      if (!Keys(p, {"camera", "width", "height", "framerate", "bitrate_bps", "duration_seconds"})) return std::unexpected("视频参数字段非法");
      const auto camera = Integer(p, "camera", 0, 3), width = Integer(p, "width", 64, 1920);
      const auto height = Integer(p, "height", 64, 1080), fps = Integer(p, "framerate", 1, 30);
      const auto bitrate = Integer(p, "bitrate_bps", 100000, 8000000);
      const auto duration = Integer(p, "duration_seconds", 1, test ? 10 : 1800);
      if (!camera || !width || !height || !fps || !bitrate || !duration || *width % 2 || *height % 2) return std::unexpected("视频参数超出范围");
      result.video.kind = start ? video::Kind::kPublish : video::Kind::kTest;
      result.video.parameters = {*camera, *width, *height, *fps, *bitrate};
      result.video.duration = std::chrono::seconds{*duration};
    }
    if (start) {
      const auto& destination = root.at("destination");
      if (!Keys(destination, {"host", "port", "stream_path"}) || !destination.at("host").is_string() ||
          !destination.at("stream_path").is_string()) return std::unexpected("视频目的地字段非法");
      const auto port = Integer(destination, "port", 1, 65535);
      result.video.destination.host = destination.at("host").get<std::string>();
      result.video.destination.stream_path = destination.at("stream_path").get<std::string>();
      if (!port || !Host(result.video.destination.host) || !StreamPath(result.video.destination.stream_path)) return std::unexpected("视频目的地取值非法");
      result.video.destination.port = *port;
    }
    root.erase("command_id");
    result.comparison = root.dump();
    return result;
  } catch (const Json::exception&) {
    return std::unexpected("视频动作JSON或字段非法");
  }
}
}  // namespace experiment
