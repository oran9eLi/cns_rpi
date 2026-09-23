/**
 * @file pi_link_inspection.cpp
 * @brief 固定只读实验动作的设备侧协议处理。
 *
 * @details 只消费调用方传入的内存事实，不持有串口或 MQTT 客户端；业务终态缓存
 * 仅用于本次进程内的只读动作，不能作为将来可变参数动作的崩溃一致性保证。
 */

#include "experiment/pi_link_inspection.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <expected>
#include <ranges>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "mqtt/topic.hpp"
#include "protocol/identity.hpp"

namespace experiment {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaximumPayloadBytes = 4096;
constexpr std::size_t kMaximumActions = 128;
constexpr auto kRetention = std::chrono::seconds(300);

struct Request {
  std::string command_id;
  std::string request_id;
  std::string device_id;
  std::string session_id;
  std::string action_id;
  std::uint64_t lease_version;
  std::string operation;
  std::string expires_at;
  PiLinkInspector::WallClock::time_point expiry;
};

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

std::optional<int> Digits(std::string_view text) {
  int result = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return result;
}

std::optional<PiLinkInspector::WallClock::time_point> ParseUtcMillis(
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
      *hour > 23 || *minute > 59 || *second > 59) {
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

std::string FormatUtcMillis(PiLinkInspector::WallClock::time_point now) {
  const auto rounded = std::chrono::floor<std::chrono::milliseconds>(now);
  const auto day = std::chrono::floor<std::chrono::days>(rounded);
  const std::chrono::year_month_day date{day};
  const std::chrono::hh_mm_ss time{rounded - day};
  char output[32];
  std::snprintf(output, sizeof(output), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
                static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                static_cast<unsigned>(date.day()),
                static_cast<long long>(time.hours().count()),
                static_cast<long long>(time.minutes().count()),
                static_cast<long long>(time.seconds().count()),
                static_cast<long long>(time.subseconds().count()));
  return output;
}

bool HasExactKeys(const Json& object, std::initializer_list<std::string_view> keys) {
  if (!object.is_object() || object.size() != keys.size()) return false;
  return std::ranges::all_of(keys, [&](std::string_view key) {
    return object.contains(std::string(key));
  });
}

std::expected<Request, std::string> Parse(std::string_view payload) {
  if (payload.size() > kMaximumPayloadBytes) {
    return std::unexpected("实验动作载荷超过4096字节");
  }
  Json root;
  try {
    root = Json::parse(payload.begin(), payload.end());
    if (!HasExactKeys(root, {"schema_version", "command_id", "request_id",
                             "target", "session_id", "action_id", "lease_version",
                             "operation", "expires_at"}) ||
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
        !root.at("expires_at").is_string()) {
      return std::unexpected("实验动作字段或类型非法");
    }
    const auto& lease_value = root.at("lease_version");
    if ((lease_value.is_number_unsigned() && lease_value.get<std::uint64_t>() == 0) ||
        (!lease_value.is_number_unsigned() && lease_value.get<std::int64_t>() <= 0)) {
      return std::unexpected("实验动作租约版本非法");
    }
    Request request{
        .command_id = root.at("command_id").get<std::string>(),
        .request_id = root.at("request_id").get<std::string>(),
        .device_id = root.at("target").at("device_id").get<std::string>(),
        .session_id = root.at("session_id").get<std::string>(),
        .action_id = root.at("action_id").get<std::string>(),
        .lease_version = lease_value.get<std::uint64_t>(),
        .operation = root.at("operation").get<std::string>(),
        .expires_at = root.at("expires_at").get<std::string>(),
        .expiry = {},
    };
    if (!IsUuid(request.command_id) || !IsUuid(request.request_id) ||
        !IsUuid(request.session_id) || !IsUuid(request.action_id) ||
        request.request_id != request.action_id ||
        !protocol::IsValidUasId(request.device_id) ||
        request.operation.empty() || request.operation.size() > 64) {
      return std::unexpected("实验动作身份或操作名非法");
    }
    const auto expiry = ParseUtcMillis(request.expires_at);
    if (!expiry) return std::unexpected("实验动作过期时间格式非法");
    request.expiry = *expiry;
    return request;
  } catch (const Json::exception&) {
    return std::unexpected("实验动作不是合法JSON或字段类型非法");
  }
}

std::string CompareContent(const Request& request) {
  return Json{{"target.device_id", request.device_id},
              {"session_id", request.session_id},
              {"request_id", request.request_id},
              {"lease_version", request.lease_version},
              {"operation", request.operation},
              {"expires_at", request.expires_at}}.dump();
}

std::string_view Name(runtime_status::ControlStatus status) {
  return status == runtime_status::ControlStatus::kOnline ? "online" : "offline";
}

std::string_view Name(runtime_status::BusinessStatus status) {
  switch (status) {
    case runtime_status::BusinessStatus::kOnline: return "online";
    case runtime_status::BusinessStatus::kOffline: return "offline";
    case runtime_status::BusinessStatus::kUnknown: return "unknown";
  }
  return "unknown";
}

std::string_view Name(runtime_status::IdentityStatus status) {
  switch (status) {
    case runtime_status::IdentityStatus::kVerified: return "verified";
    case runtime_status::IdentityStatus::kCached: return "cached";
    case runtime_status::IdentityStatus::kConflict: return "conflict";
    case runtime_status::IdentityStatus::kUnbound: return "unbound";
  }
  return "unbound";
}

Json BaseAck(const Request& request, PiLinkInspector::WallClock::time_point observed_at) {
  return {{"schema_version", 1}, {"command_id", request.command_id},
          {"session_id", request.session_id}, {"action_id", request.action_id},
          {"operation", request.operation},
          {"observed_at", FormatUtcMillis(observed_at)}};
}

Json Rejected(const Request& request, PiLinkInspector::WallClock::time_point observed_at,
              std::string_view code) {
  auto ack = BaseAck(request, observed_at);
  ack["status"] = "rejected";
  ack["error_code"] = code;
  return ack;
}

Publication ToPublication(std::string_view topic_namespace, std::string_view bound_device_id,
                          const Json& ack) {
  return {.topic = mqtt::BuildExperimentAckTopic(std::string(topic_namespace),
                                                 std::string(bound_device_id)),
          .payload = ack.dump(), .qos = 2, .retain = false};
}

}  // namespace

std::optional<bool> SerialEndpointFact(bool active_link_open) {
  return active_link_open ? std::optional<bool>{true} : std::nullopt;
}

std::optional<std::pair<std::string, int>> DeviceSubscription(
    const std::string& topic_namespace,
    const std::optional<device::Binding>& persisted_binding) {
  if (!persisted_binding || persisted_binding->device_type != device::Type::kCnsBox ||
      !protocol::IsValidUasId(persisted_binding->device_id)) {
    return std::nullopt;
  }
  return std::pair{mqtt::BuildExperimentSetTopic(
                       topic_namespace, persisted_binding->device_id), 2};
}

PiLinkInspector::PiLinkInspector(std::string topic_namespace)
    : topic_namespace_(std::move(topic_namespace)) {}

Outcome PiLinkInspector::Handle(std::string_view topic, std::string_view payload,
                                const std::optional<device::Binding>& persisted_binding,
                                const Sampler& sample, WallClock::time_point wall_now,
                                SteadyClock::time_point steady_now) {
  if (!persisted_binding || persisted_binding->device_type != device::Type::kCnsBox ||
      !protocol::IsValidUasId(persisted_binding->device_id)) {
    return {.publication = std::nullopt,
            .diagnostic = "实验动作无合法持久化主控箱身份，拒绝认领Topic"};
  }
  if (topic != mqtt::BuildExperimentSetTopic(topic_namespace_, persisted_binding->device_id)) {
    return {.publication = std::nullopt,
            .diagnostic = "实验动作Topic与持久化设备身份不一致"};
  }
  const auto parsed = Parse(payload);
  if (!parsed) return {.publication = std::nullopt, .diagnostic = parsed.error()};
  const Request& request = *parsed;
  const auto comparison = CompareContent(request);
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (steady_now - it->terminated_at > kRetention) it = entries_.erase(it);
    else ++it;
  }
  const auto existing = std::ranges::find_if(entries_, [&](const Entry& entry) {
    return entry.action_id == request.action_id;
  });
  if (existing != entries_.end()) {
    if (existing->comparison != comparison) {
      return {.publication = ToPublication(
                  topic_namespace_, persisted_binding->device_id,
                  Rejected(request, wall_now, "duplicate_conflict")),
              .diagnostic = "实验动作ID重复但内容冲突"};
    }
    auto ack = Json::parse(existing->terminal_payload);
    ack["command_id"] = request.command_id;
    return {.publication = ToPublication(topic_namespace_,
                                         persisted_binding->device_id, ack),
            .diagnostic = {}};
  }

  Json ack;
  if (request.device_id != persisted_binding->device_id) {
    ack = Rejected(request, wall_now, "identity_unavailable");
  } else if (request.operation != "inspect_pi_link") {
    ack = Rejected(request, wall_now, "unsupported_operation");
  } else if (wall_now >= request.expiry) {
    ack = Rejected(request, wall_now, "action_expired");
  } else {
    const auto observation = sample ? sample() : std::nullopt;
    if (!observation || !observation->mqtt_connected ||
        observation->runtime.control_status != runtime_status::ControlStatus::kOnline) {
      ack = Rejected(request, wall_now, "observation_unavailable");
    } else if (observation->runtime.device_id != persisted_binding->device_id ||
               observation->runtime.identity_status == runtime_status::IdentityStatus::kConflict ||
               observation->runtime.identity_status == runtime_status::IdentityStatus::kUnbound) {
      ack = Rejected(request, wall_now, "identity_unavailable");
    } else {
      ack = BaseAck(request, wall_now);
      ack["status"] = "completed";
      ack["fact"] = {
          {"control_status", Name(observation->runtime.control_status)},
          {"identity_status", Name(observation->runtime.identity_status)},
          {"business_status", Name(observation->runtime.business_status)},
          {"serial_port_open", observation->serial_port_open
                                   ? Json(*observation->serial_port_open)
                                   : Json("unknown")},
      };
    }
  }
  const auto publication = ToPublication(topic_namespace_,
                                          persisted_binding->device_id, ack);
  if (publication.payload.size() > kMaximumPayloadBytes) {
    return {.publication = std::nullopt,
            .diagnostic = "实验动作ACK超过4096字节，拒绝发布"};
  }
  if (entries_.size() >= kMaximumActions) entries_.pop_front();
  entries_.push_back({.action_id = request.action_id,
                      .comparison = comparison,
                      .terminal_payload = publication.payload,
                      .terminated_at = steady_now});
  return {.publication = publication, .diagnostic = {}};
}

std::size_t PiLinkInspector::CachedActionCount() const { return entries_.size(); }

bool AckOutbox::Enqueue(Publication publication) {
  if (pending_.size() >= kCapacity) return false;
  pending_.push_back(std::move(publication));
  return true;
}

const Publication* AckOutbox::Front() const {
  return pending_.empty() ? nullptr : &pending_.front();
}

void AckOutbox::ConfirmFront() {
  if (!pending_.empty()) pending_.pop_front();
}

bool AckOutbox::FlushOne(const std::function<bool(const Publication&)>& publish) {
  if (pending_.empty()) return true;
  if (!publish(pending_.front())) return false;
  pending_.pop_front();
  return true;
}

std::size_t AckOutbox::Size() const { return pending_.size(); }

bool FeedRecoveredAckWhenIdle(AckOutbox& outbox,
                              std::deque<Publication>& recovered) {
  if (outbox.Size() != 0 || recovered.empty()) return false;
  if (!outbox.Enqueue(recovered.front())) return false;
  recovered.pop_front();
  return true;
}

}  // namespace experiment
