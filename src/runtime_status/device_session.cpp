/**
 * @file device_session.cpp
 * @brief device_session.hpp 的实现。
 */

#include "runtime_status/device_session.hpp"

#include <utility>

namespace runtime_status {

DeviceSession::DeviceSession(
    std::optional<device::Binding> persisted_binding,
    Clock::time_point started_at)
    : started_at_(started_at),
      active_binding_(std::move(persisted_binding)),
      has_persisted_binding_(active_binding_.has_value()) {
  if (active_binding_) {
    tracker_.emplace(
        active_binding_->device_id,
        std::optional<std::string>{active_binding_->device_id}, started_at_);
  }
}

const device::Binding* DeviceSession::ActiveBinding() const {
  return active_binding_ ? &*active_binding_ : nullptr;
}

bool DeviceSession::HasPersistedBinding() const {
  return has_persisted_binding_;
}

IdentityObservation DeviceSession::ObserveIdentity(
    const device::Binding& current) {
  if (!active_binding_) {
    active_binding_ = current;
    tracker_.emplace(current.device_id, std::nullopt, started_at_);
  }

  if (!has_persisted_binding_) {
    if (current.device_id == active_binding_->device_id && tracker_) {
      tracker_->ObserveIdentity(current.device_id);
    }
    return IdentityObservation::kUnbound;
  }

  tracker_->ObserveIdentity(current.device_id);
  return current == *active_binding_ ? IdentityObservation::kVerified
                                     : IdentityObservation::kConflict;
}

void DeviceSession::ConfirmPersistedBinding() {
  if (!active_binding_ || !tracker_) {
    return;
  }
  has_persisted_binding_ = true;
  tracker_->ConfirmPersistedIdentity(active_binding_->device_id);
}

void DeviceSession::SetLinkAvailable(bool available) {
  link_available_ = available;
}

void DeviceSession::ObserveBusinessFrame(Clock::time_point now) {
  if (tracker_) {
    tracker_->ObserveBusinessFrame(now);
  }
}

void DeviceSession::Tick(Clock::time_point now) {
  if (tracker_) {
    tracker_->Tick(now);
  }
}

std::optional<Snapshot> DeviceSession::CurrentOnlineStatus() const {
  if (!tracker_) {
    return std::nullopt;
  }
  return tracker_->CurrentOnlineSnapshot();
}

bool DeviceSession::CanSendDeviceCommands() const {
  const auto status = CurrentOnlineStatus();
  return link_available_ && status &&
         status->identity_status == IdentityStatus::kVerified;
}

bool DeviceSession::CanPublishTelemetry() const {
  const auto status = CurrentOnlineStatus();
  return CanSendDeviceCommands() && status &&
         status->business_status == BusinessStatus::kOnline;
}

}  // namespace runtime_status
