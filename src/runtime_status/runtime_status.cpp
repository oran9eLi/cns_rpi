/**
 * @file runtime_status.cpp
 * @brief runtime_status.hpp 的实现。
 */

#include "runtime_status/runtime_status.hpp"

#include <utility>

namespace runtime_status {

namespace {

constexpr auto kBusinessSilenceTimeout = std::chrono::seconds(10);

}  // namespace

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

}  // namespace runtime_status
