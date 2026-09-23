/**
 * @file uart_echo_request.cpp
 * @brief H1 受限文本及设备实验请求校验；不产生串口副作用。
 */

#include "experiment/uart_echo_request.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <expected>
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

bool IsWhitespace(std::uint32_t codepoint) {
  return (codepoint >= 0x0009 && codepoint <= 0x000D) ||
         codepoint == 0x0020 || codepoint == 0x0085 ||
         codepoint == 0x00A0 || codepoint == 0x1680 ||
         (codepoint >= 0x2000 && codepoint <= 0x200A) ||
         codepoint == 0x2028 || codepoint == 0x2029 ||
         codepoint == 0x202F || codepoint == 0x205F ||
         codepoint == 0x3000;
}

bool IsAllowedText(std::string_view text) {
  if (text.empty() || text.size() > 48) return false;
  bool has_non_whitespace = false;
  for (std::size_t index = 0; index < text.size();) {
    const auto lead = static_cast<std::uint8_t>(text[index]);
    std::uint32_t codepoint = 0;
    std::size_t length = 0;
    if (lead <= 0x7F) {
      codepoint = lead;
      length = 1;
    } else if (lead >= 0xC2 && lead <= 0xDF) {
      codepoint = lead & 0x1F;
      length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      codepoint = lead & 0x0F;
      length = 3;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      codepoint = lead & 0x07;
      length = 4;
    } else {
      return false;
    }
    if (index + length > text.size()) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<std::uint8_t>(text[index + offset]);
      if ((next & 0xC0) != 0x80) return false;
      codepoint = (codepoint << 6) | (next & 0x3F);
    }
    if ((length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
        codepoint > 0x10FFFF || codepoint <= 0x001F ||
        (codepoint >= 0x007F && codepoint <= 0x009F)) {
      return false;
    }
    has_non_whitespace |= !IsWhitespace(codepoint);
    index += length;
  }
  return has_non_whitespace;
}

}  // namespace

bool IsUartEchoOperation(std::string_view payload) {
  if (payload.size() > kMaximumPayloadBytes) return false;
  const Json root = Json::parse(payload.begin(), payload.end(), nullptr, false);
  return root.is_object() && root.contains("operation") &&
         root.at("operation").is_string() &&
         root.at("operation").get<std::string>() == "uart_echo";
}

std::expected<UartEchoRequest, std::string> ParseUartEchoRequest(
    std::string_view payload, std::string_view expected_device_id,
    std::chrono::system_clock::time_point now) {
  if (payload.size() > kMaximumPayloadBytes) {
    return std::unexpected("回显请求超过4096字节");
  }
  try {
    const Json root = Json::parse(payload.begin(), payload.end());
    if (!HasExactKeys(root, {"schema_version", "command_id", "request_id",
                             "target", "session_id", "action_id", "lease_version",
                             "operation", "text", "expires_at"}) ||
        !HasExactKeys(root.at("target"), {"device_id"}) ||
        !root.at("schema_version").is_number_integer() ||
        root.at("schema_version").get<int>() != 1 ||
        !root.at("command_id").is_string() ||
        !root.at("request_id").is_string() ||
        !root.at("target").at("device_id").is_string() ||
        !root.at("session_id").is_string() ||
        !root.at("action_id").is_string() ||
        !root.at("lease_version").is_number_integer() ||
        !root.at("operation").is_string() ||
        !root.at("text").is_string() ||
        !root.at("expires_at").is_string()) {
      return std::unexpected("回显请求字段或类型非法");
    }
    const auto& lease = root.at("lease_version");
    if ((lease.is_number_unsigned() && lease.get<std::uint64_t>() == 0) ||
        (!lease.is_number_unsigned() && lease.get<std::int64_t>() <= 0)) {
      return std::unexpected("回显请求租约版本非法");
    }

    UartEchoRequest request{
        .command_id = root.at("command_id").get<std::string>(),
        .request_id = root.at("request_id").get<std::string>(),
        .device_id = root.at("target").at("device_id").get<std::string>(),
        .session_id = root.at("session_id").get<std::string>(),
        .action_id = root.at("action_id").get<std::string>(),
        .lease_version = lease.get<std::uint64_t>(),
        .expires_at = root.at("expires_at").get<std::string>(),
        .expiry = {},
        .text_bytes = {},
    };
    if (!IsUuid(request.command_id) || !IsUuid(request.request_id) ||
        !IsUuid(request.session_id) || !IsUuid(request.action_id) ||
        request.action_id != request.request_id ||
        !protocol::IsValidUasId(request.device_id) ||
        request.device_id != expected_device_id ||
        root.at("operation").get<std::string>() != "uart_echo") {
      return std::unexpected("回显请求身份或操作非法");
    }
    const auto expiry = ParseUtcMillis(request.expires_at);
    if (!expiry || now >= *expiry) {
      return std::unexpected("回显请求已过期或有效期非法");
    }
    request.expiry = *expiry;
    const auto text = root.at("text").get<std::string>();
    if (!IsAllowedText(text)) {
      return std::unexpected("回显文本不满足UTF8长度、空白或控制字符限制");
    }
    request.text_bytes.assign(text.begin(), text.end());
    return request;
  } catch (const Json::exception&) {
    return std::unexpected("回显请求不是合法JSON或字段类型非法");
  }
}

}  // namespace experiment
