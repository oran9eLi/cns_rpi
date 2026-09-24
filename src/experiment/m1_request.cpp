/**
 * @file m1_request.cpp
 * @brief M1 三种受限实验请求的本机校验，不产生物理副作用。
 */

#include "experiment/m1_request.hpp"

#include <charconv>
#include <chrono>
#include <optional>
#include <ranges>
#include <string>

#include <nlohmann/json.hpp>

#include "protocol/identity.hpp"

namespace experiment {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaximumPayloadBytes = 4096;

bool HasExactKeys(const Json& object,
                  std::initializer_list<std::string_view> keys) {
  return object.is_object() && object.size() == keys.size() &&
         std::ranges::all_of(keys, [&](std::string_view key) {
           return object.contains(std::string(key));
         });
}

bool IsUuid(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
    } else if (!((value[index] >= '0' && value[index] <= '9') ||
                 (value[index] >= 'a' && value[index] <= 'f') ||
                 (value[index] >= 'A' && value[index] <= 'F'))) {
      return false;
    }
  }
  return true;
}

std::optional<int> Digits(std::string_view value) {
  int result = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

std::optional<std::chrono::system_clock::time_point> ParseUtcMillis(
    std::string_view value) {
  if (value.size() != 24 || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
      value[19] != '.' || value[23] != 'Z') {
    return std::nullopt;
  }
  const auto year = Digits(value.substr(0, 4));
  const auto month = Digits(value.substr(5, 2));
  const auto day = Digits(value.substr(8, 2));
  const auto hour = Digits(value.substr(11, 2));
  const auto minute = Digits(value.substr(14, 2));
  const auto second = Digits(value.substr(17, 2));
  const auto millisecond = Digits(value.substr(20, 3));
  if (!year || !month || !day || !hour || !minute || !second || !millisecond ||
      *hour > 23 || *minute > 59 || *second > 59 || *millisecond > 999) {
    return std::nullopt;
  }
  const std::chrono::year_month_day date{
      std::chrono::year{*year}, std::chrono::month{static_cast<unsigned>(*month)},
      std::chrono::day{static_cast<unsigned>(*day)}};
  if (!date.ok()) return std::nullopt;
  return std::chrono::sys_days{date} + std::chrono::hours{*hour} +
         std::chrono::minutes{*minute} + std::chrono::seconds{*second} +
         std::chrono::milliseconds{*millisecond};
}

std::optional<M1Operation> ParseOperation(const Json& value) {
  if (!value.is_string()) return std::nullopt;
  const auto operation = value.get<std::string>();
  if (operation == "set_f407_baud") return M1Operation::kSetF407Baud;
  if (operation == "set_pi_baud") return M1Operation::kSetPiBaud;
  if (operation == "uart_probe") return M1Operation::kUartProbe;
  return std::nullopt;
}
}  // namespace

bool IsM1Operation(std::string_view payload) {
  if (payload.size() > kMaximumPayloadBytes) return false;
  const Json root = Json::parse(payload.begin(), payload.end(), nullptr, false);
  return root.is_object() && root.contains("operation") &&
         ParseOperation(root.at("operation")).has_value();
}

std::expected<M1Request, std::string> ParseM1Request(
    std::string_view payload, std::string_view expected_device_id) {
  if (payload.size() > kMaximumPayloadBytes) {
    return std::unexpected("M1请求超过4096字节");
  }
  try {
    const Json root = Json::parse(payload.begin(), payload.end());
    if (!root.is_object() || !root.contains("operation")) {
      return std::unexpected("M1请求缺少操作");
    }
    const auto operation = ParseOperation(root.at("operation"));
    if (!operation) return std::unexpected("M1操作非法");
    const bool has_baud = *operation != M1Operation::kUartProbe;
    if (!(has_baud
              ? HasExactKeys(root, {"schema_version", "command_id", "request_id",
                                    "target", "session_id", "action_id",
                                    "lease_version", "operation", "baud_rate",
                                    "expires_at"})
              : HasExactKeys(root, {"schema_version", "command_id", "request_id",
                                    "target", "session_id", "action_id",
                                    "lease_version", "operation", "expires_at"})) ||
        !HasExactKeys(root.at("target"), {"device_id"}) ||
        !root.at("schema_version").is_number_integer() ||
        root.at("schema_version").get<int>() != 1 ||
        !root.at("command_id").is_string() ||
        !root.at("request_id").is_string() ||
        !root.at("target").at("device_id").is_string() ||
        !root.at("session_id").is_string() ||
        !root.at("action_id").is_string() ||
        !root.at("lease_version").is_number_integer() ||
        !root.at("expires_at").is_string() ||
        (has_baud && !root.at("baud_rate").is_number_integer())) {
      return std::unexpected("M1请求字段或类型非法");
    }
    const auto& lease = root.at("lease_version");
    if ((lease.is_number_unsigned() && lease.get<std::uint64_t>() == 0) ||
        (!lease.is_number_unsigned() && lease.get<std::int64_t>() <= 0)) {
      return std::unexpected("M1租约版本非法");
    }
    if (has_baud && root.at("baud_rate") != 57600 &&
        root.at("baud_rate") != 115200) {
      return std::unexpected("M1目标波特率不在白名单内");
    }
    M1Request request{
        .command_id = root.at("command_id").get<std::string>(),
        .request_id = root.at("request_id").get<std::string>(),
        .device_id = root.at("target").at("device_id").get<std::string>(),
        .session_id = root.at("session_id").get<std::string>(),
        .action_id = root.at("action_id").get<std::string>(),
        .lease_version = lease.get<std::uint64_t>(),
        .operation = *operation,
        .baud_rate = has_baud ? std::optional<int>{root.at("baud_rate").get<int>()}
                              : std::nullopt,
        .expires_at = root.at("expires_at").get<std::string>(),
        .expiry = {},
    };
    if (!IsUuid(request.command_id) || !IsUuid(request.request_id) ||
        !IsUuid(request.session_id) || !IsUuid(request.action_id) ||
        request.action_id != request.request_id ||
        !protocol::IsValidUasId(request.device_id) ||
        request.device_id != expected_device_id) {
      return std::unexpected("M1请求身份非法");
    }
    const auto expiry = ParseUtcMillis(request.expires_at);
    if (!expiry) return std::unexpected("M1有效期格式非法");
    request.expiry = *expiry;
    if ((*operation == M1Operation::kSetF407Baud && request.baud_rate != 57600) ||
        (*operation == M1Operation::kSetPiBaud && request.baud_rate != 57600 &&
         request.baud_rate != 115200)) {
      return std::unexpected("M1目标波特率不在白名单内");
    }
    return request;
  } catch (const Json::exception&) {
    return std::unexpected("M1请求不是合法JSON或字段类型非法");
  }
}

}  // namespace experiment
