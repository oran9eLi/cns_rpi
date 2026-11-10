/**
 * @file gnss_request.cpp
 * @brief GNSS动作的入口校验；不执行串口操作或发布 ACK。
 */
#include "experiment/gnss_request.hpp"

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
constexpr std::array<std::string_view, 3> kOperations{"gnss_capture_start", "gnss_capture_stop", "gnss_status"};

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

std::optional<int> Integer(const Json& value, int low, int high) {
  if (!value.is_number_integer()) return std::nullopt;
  if (value.is_number_unsigned()) {
    if (value.get<std::uint64_t>() > static_cast<std::uint64_t>(high)) return std::nullopt;
  } else if (value.get<std::int64_t>() < low || value.get<std::int64_t>() > high) {
    return std::nullopt;
  }
  const int result = value.get<int>();
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
}  // namespace

bool IsGnssOperation(std::string_view payload) {
  if (payload.size() > 4096) return false;
  const auto value = Json::parse(payload, nullptr, false);
  if (!value.is_object() || !value.contains("operation") || !value.at("operation").is_string()) return false;
  return std::ranges::find(kOperations, value.at("operation").get<std::string>()) != kOperations.end();
}

std::expected<GnssRequest, std::string> ParseGnssRequest(std::string_view payload) {
  if (payload.size() > 4096) return std::unexpected("GNSS动作载荷超过4096字节");
  try {
    auto root = Json::parse(payload);
    if (!root.is_object()) return std::unexpected("GNSS动作必须为对象");
    for (const auto key : {"command_id", "request_id", "session_id", "action_id", "operation", "expires_at"}) {
      if (!root.contains(key) || !root.at(key).is_string()) return std::unexpected("GNSS动作字段类型非法");
    }
    GnssRequest result;
    result.operation = root.at("operation").get<std::string>();
    if (std::ranges::find(kOperations, result.operation) == kOperations.end()) return std::unexpected("未知GNSS动作");
    const bool capture = result.operation != "gnss_status";
    constexpr std::array<std::string_view, 9> base_keys{"schema_version", "command_id", "request_id",
        "target", "session_id", "action_id", "lease_version", "operation", "expires_at"};
    if (root.size() != 9 + static_cast<std::size_t>(capture) ||
        !std::ranges::all_of(base_keys, [&](auto key) { return root.contains(key); }) ||
        !Integer(root.at("schema_version"), 1, 1) || !Keys(root.at("target"), {"device_id"}) ||
        !root.at("target").at("device_id").is_string()) return std::unexpected("GNSS动作字段非法");
    result.command_id = root.at("command_id").get<std::string>();
    result.device_id = root.at("target").at("device_id").get<std::string>();
    result.session_id = root.at("session_id").get<std::string>();
    result.action_id = root.at("action_id").get<std::string>();
    const auto& lease = root.at("lease_version");
    if (!Uuid(result.command_id) || !Uuid(result.session_id) || !Uuid(result.action_id) ||
        root.at("request_id").get<std::string>() != result.action_id || !protocol::IsValidUasId(result.device_id) ||
        !lease.is_number_integer() ||
        (!lease.is_number_unsigned() && lease.get<std::int64_t>() <= 0) ||
        (lease.is_number_unsigned() && lease.get<std::uint64_t>() == 0)) return std::unexpected("GNSS动作身份或租约非法");
    result.lease_version = lease.get<std::uint64_t>();
    const auto expiry = Utc(root.at("expires_at").get<std::string>());
    if (!expiry) return std::unexpected("GNSS动作过期时间非法");
    result.expiry = *expiry;
    if (capture) {
      const auto& p = root.at("parameters");
      const bool start = result.operation == "gnss_capture_start";
      if (!(start ? Keys(p, {"capture_id", "duration_seconds"}) : Keys(p, {"capture_id"})) ||
          !p.at("capture_id").is_string() || !Uuid(p.at("capture_id").get<std::string>()))
        return std::unexpected("GNSS采集参数非法");
      result.capture_id = p.at("capture_id").get<std::string>();
      if (start) {
        const auto duration = Integer(p.at("duration_seconds"), 1, 1800);
        if (!duration) return std::unexpected("GNSS采集时长必须为1至1800秒整数");
        result.duration_seconds = static_cast<std::uint32_t>(*duration);
      }
    }
    root.erase("command_id");
    result.comparison = root.dump();
    return result;
  } catch (const Json::exception&) {
    return std::unexpected("GNSS动作JSON或字段非法");
  }
}
}  // namespace experiment
