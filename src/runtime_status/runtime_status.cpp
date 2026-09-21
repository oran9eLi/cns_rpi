/**
 * @file runtime_status.cpp
 * @brief runtime_status.hpp 的实现。
 */

#include "runtime_status/runtime_status.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "mqtt/topic.hpp"

namespace runtime_status {

namespace {

constexpr int kSchemaVersion = 1;
constexpr auto kBusinessSilenceTimeout = std::chrono::seconds(10);

bool IsValidDeviceId(const std::string& value) {
  return !value.empty() && value.size() <= 20 &&
         std::ranges::all_of(value, [](unsigned char ch) {
           return std::isalnum(ch) != 0 || ch == '.' || ch == '_' ||
                  ch == ':' || ch == '-';
         });
}

std::string_view Name(ControlStatus status) {
  return status == ControlStatus::kOnline ? "online" : "offline";
}

std::string_view Name(BusinessStatus status) {
  switch (status) {
    case BusinessStatus::kOnline:
      return "online";
    case BusinessStatus::kOffline:
      return "offline";
    case BusinessStatus::kUnknown:
      return "unknown";
  }
  return "unknown";
}

std::string_view Name(IdentityStatus status) {
  switch (status) {
    case IdentityStatus::kVerified:
      return "verified";
    case IdentityStatus::kCached:
      return "cached";
    case IdentityStatus::kConflict:
      return "conflict";
    case IdentityStatus::kUnbound:
      return "unbound";
  }
  return "unbound";
}

bool IsLegalCombination(const Snapshot& snapshot) {
  if (snapshot.control_status == ControlStatus::kOnline) {
    return true;
  }
  return snapshot.business_status == BusinessStatus::kUnknown &&
         (snapshot.identity_status == IdentityStatus::kCached ||
          snapshot.identity_status == IdentityStatus::kUnbound);
}

}  // namespace

std::expected<Publication, std::string> BuildPublication(
    const std::string& topic_namespace, const std::string& topic_suffix,
    const Snapshot& snapshot) {
  if (!IsValidDeviceId(snapshot.device_id)) {
    return std::unexpected("运行三态device_id非法");
  }
  if (!IsLegalCombination(snapshot)) {
    return std::unexpected("运行三态状态组合非法");
  }

  const nlohmann::json payload{
      {"schema_version", kSchemaVersion},
      {"device_id", snapshot.device_id},
      {"control_status", Name(snapshot.control_status)},
      {"business_status", Name(snapshot.business_status)},
      {"identity_status", Name(snapshot.identity_status)},
  };
  return Publication{
      .topic = mqtt::BuildRuntimeStatusTopic(
          topic_namespace, snapshot.device_id, topic_suffix),
      .payload = payload.dump(),
      .qos = 1,
      .retain = true,
  };
}

std::expected<Publication, std::string> BuildLastWillPublication(
    const std::string& topic_namespace, const std::string& topic_suffix,
    const std::string& device_id, bool has_persisted_binding) {
  return BuildPublication(
      topic_namespace, topic_suffix,
      Snapshot{
          .device_id = device_id,
          .control_status = ControlStatus::kOffline,
          .business_status = BusinessStatus::kUnknown,
          .identity_status = has_persisted_binding
                                 ? IdentityStatus::kCached
                                 : IdentityStatus::kUnbound,
      });
}

bool LastWillNeedsRefresh(std::optional<bool> configured_has_binding,
                          bool current_has_binding) {
  return configured_has_binding &&
         *configured_has_binding != current_has_binding;
}

Tracker::Tracker(std::string device_id,
                 std::optional<std::string> persisted_device_id,
                 Clock::time_point started_at)
    : device_id_(std::move(device_id)),
      persisted_device_id_(std::move(persisted_device_id)),
      started_at_(started_at),
      identity_status_(persisted_device_id_ ? IdentityStatus::kCached
                                            : IdentityStatus::kUnbound) {}

void Tracker::ObserveBusinessFrame(Clock::time_point now) {
  last_business_frame_ = now;
  business_status_ = BusinessStatus::kOnline;
}

void Tracker::ObserveIdentity(const std::string& current_device_id) {
  if (!persisted_device_id_) {
    identity_status_ = IdentityStatus::kUnbound;
  } else if (*persisted_device_id_ == current_device_id) {
    identity_status_ = IdentityStatus::kVerified;
  } else {
    identity_status_ = IdentityStatus::kConflict;
  }
}

void Tracker::ConfirmPersistedIdentity(const std::string& persisted_device_id) {
  persisted_device_id_ = persisted_device_id;
  identity_status_ = persisted_device_id == device_id_
                         ? IdentityStatus::kVerified
                         : IdentityStatus::kConflict;
}

void Tracker::InvalidateCurrentIdentity() {
  identity_status_ = persisted_device_id_ ? IdentityStatus::kCached
                                          : IdentityStatus::kUnbound;
}

void Tracker::Tick(Clock::time_point now) {
  const auto reference = last_business_frame_.value_or(started_at_);
  if (now - reference >= kBusinessSilenceTimeout) {
    business_status_ = BusinessStatus::kOffline;
  }
}

Snapshot Tracker::CurrentOnlineSnapshot() const {
  return {
      .device_id = device_id_,
      .control_status = ControlStatus::kOnline,
      .business_status = business_status_,
      .identity_status = identity_status_,
  };
}

bool PublicationState::ShouldPublish(bool mqtt_connected,
                                     std::uint64_t connection_generation,
                                     const Snapshot& current) {
  if (mqtt_connected && connection_generation != 0 &&
      connection_generation != observed_connection_generation_) {
    connection_requires_publish_ = true;
    observed_connection_generation_ = connection_generation;
  }
  return mqtt_connected &&
         (connection_requires_publish_ || !last_published_ ||
          *last_published_ != current);
}

void PublicationState::MarkPublished(const Snapshot& snapshot) {
  last_published_ = snapshot;
  connection_requires_publish_ = false;
}

}  // namespace runtime_status
