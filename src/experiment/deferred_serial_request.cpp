/**
 * @file deferred_serial_request.cpp
 * @brief 单槽待发实验请求的去重与移交。
 */

#include "experiment/deferred_serial_request.hpp"

#include <cstdio>
#include <nlohmann/json.hpp>
#include <type_traits>
#include <utility>

#include "mqtt/topic.hpp"

namespace experiment {
namespace {

std::string FormatTime(std::chrono::system_clock::time_point now) {
  const auto rounded = std::chrono::floor<std::chrono::milliseconds>(now);
  const auto day = std::chrono::floor<std::chrono::days>(rounded);
  const std::chrono::year_month_day date{day};
  const std::chrono::hh_mm_ss time{rounded - day};
  char output[32]{};
  std::snprintf(output, sizeof(output), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
                static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                static_cast<unsigned>(date.day()),
                static_cast<long long>(time.hours().count()),
                static_cast<long long>(time.minutes().count()),
                static_cast<long long>(time.seconds().count()),
                static_cast<long long>(time.subseconds().count()));
  return output;
}

std::string_view ActionId(const DeferredSerialRequest& request) {
  return std::visit([](const auto& value) -> std::string_view {
    return value.action_id;
  }, request);
}

bool SameContent(const DeferredSerialRequest& left,
                 const DeferredSerialRequest& right) {
  if (left.index() != right.index()) return false;
  if (const auto* a = std::get_if<UartEchoRequest>(&left)) {
    const auto& b = std::get<UartEchoRequest>(right);
    return a->request_id == b.request_id && a->device_id == b.device_id &&
           a->session_id == b.session_id &&
           a->lease_version == b.lease_version &&
           a->expires_at == b.expires_at && a->text_bytes == b.text_bytes;
  }
  const auto& a = std::get<M1Request>(left);
  const auto& b = std::get<M1Request>(right);
  return a.request_id == b.request_id && a.device_id == b.device_id &&
         a.session_id == b.session_id &&
         a.lease_version == b.lease_version &&
         a.expires_at == b.expires_at && a.operation == b.operation &&
         a.baud_rate == b.baud_rate;
}

}  // namespace

DeferResult DeferredSerialRequestQueue::Enqueue(DeferredSerialRequest request) {
  if (pending_) {
    if (ActionId(*pending_) == ActionId(request)) {
      return SameContent(*pending_, request) ? DeferResult::kDuplicate
                                             : DeferResult::kConflict;
    }
    return DeferResult::kFull;
  }
  pending_ = std::move(request);
  return DeferResult::kQueued;
}

bool DeferredSerialRequestQueue::ShouldIntercept(
    bool revalidation_pending) const {
  return revalidation_pending || pending_.has_value();
}

bool DeferredSerialRequestQueue::ShouldInterceptM1(
    bool revalidation_pending, bool local_pi_baud) const {
  return pending_.has_value() ||
         (revalidation_pending && !local_pi_baud);
}

SerialRequestDecision DeferredSerialRequestQueue::Decide(
    bool revalidation_pending, bool identity_conflict,
    bool local_pi_baud) const {
  if (identity_conflict && !local_pi_baud) {
    return SerialRequestDecision::kRejectIdentity;
  }
  if (ShouldInterceptM1(revalidation_pending, local_pi_baud)) {
    return SerialRequestDecision::kDefer;
  }
  return SerialRequestDecision::kProceed;
}

const DeferredSerialRequest* DeferredSerialRequestQueue::Front() const {
  return pending_ ? &*pending_ : nullptr;
}

std::optional<DeferredSerialRequest> DeferredSerialRequestQueue::Take() {
  auto taken = std::move(pending_);
  pending_.reset();
  return taken;
}

Publication BuildDeferredRejection(
    const DeferredSerialRequest& request, const std::string& topic_namespace,
    std::string_view error_code, std::chrono::system_clock::time_point now) {
  const auto payload = std::visit([&](const auto& value) {
    std::string_view operation = "uart_echo";
    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, M1Request>) {
      switch (value.operation) {
        case M1Operation::kSetF407Baud: operation = "set_f407_baud"; break;
        case M1Operation::kSetPiBaud: operation = "set_pi_baud"; break;
        case M1Operation::kUartProbe: operation = "uart_probe"; break;
      }
    }
    return nlohmann::json{
        {"schema_version", 1}, {"command_id", value.command_id},
        {"session_id", value.session_id}, {"action_id", value.action_id},
        {"operation", operation}, {"observed_at", FormatTime(now)},
        {"status", "rejected"}, {"error_code", error_code},
    }.dump();
  }, request);
  const auto device_id = std::visit([](const auto& value) {
    return value.device_id;
  }, request);
  return {.topic = mqtt::BuildExperimentAckTopic(topic_namespace, device_id),
          .payload = payload, .qos = 2, .retain = false};
}

std::string SerializeDeferredRequest(const DeferredSerialRequest& request) {
  return std::visit([](const auto& value) {
    nlohmann::json payload{
        {"schema_version", 1}, {"command_id", value.command_id},
        {"request_id", value.request_id},
        {"target", {{"device_id", value.device_id}}},
        {"session_id", value.session_id}, {"action_id", value.action_id},
        {"lease_version", value.lease_version},
        {"expires_at", value.expires_at},
    };
    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, UartEchoRequest>) {
      payload["operation"] = "uart_echo";
      payload["text"] = std::string(value.text_bytes.begin(), value.text_bytes.end());
    } else {
      switch (value.operation) {
        case M1Operation::kSetF407Baud:
          payload["operation"] = "set_f407_baud";
          break;
        case M1Operation::kSetPiBaud:
          payload["operation"] = "set_pi_baud";
          break;
        case M1Operation::kUartProbe:
          payload["operation"] = "uart_probe";
          break;
      }
      if (value.baud_rate) payload["baud_rate"] = *value.baud_rate;
    }
    return payload.dump();
  }, request);
}

bool ShouldReplayDeferred(const DeferredSerialRequest& request,
                          std::chrono::system_clock::time_point now,
                          bool identity_ready, bool serial_idle,
                          bool mqtt_connected) {
  const auto expiry = std::visit([](const auto& value) {
    return value.expiry;
  }, request);
  return now >= expiry || (identity_ready && serial_idle && mqtt_connected);
}

}  // namespace experiment
