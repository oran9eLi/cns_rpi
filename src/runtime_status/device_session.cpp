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
  current_identity_ = current;
  if (!active_binding_) {
    active_binding_ = current;
    tracker_.emplace(current.device_id, std::nullopt, started_at_);
  }

  if (!has_persisted_binding_) {
    current_link_identity_verified_ = false;
    if (current != *active_binding_) {
      return IdentityObservation::kUnboundMismatch;
    }
    if (tracker_) {
      tracker_->ObserveIdentity(current.device_id);
    }
    return IdentityObservation::kUnbound;
  }

  tracker_->ObserveIdentity(current.device_id);
  current_link_identity_verified_ = current == *active_binding_;
  return current_link_identity_verified_ ? IdentityObservation::kVerified
                                         : IdentityObservation::kConflict;
}

bool DeviceSession::ConfirmPersistedBinding(
    const device::Binding& persisted_binding) {
  if (!active_binding_ || !tracker_ || !current_identity_ ||
      persisted_binding != *active_binding_ ||
      persisted_binding != *current_identity_) {
    return false;
  }
  has_persisted_binding_ = true;
  tracker_->ConfirmPersistedIdentity(active_binding_->device_id);
  current_link_identity_verified_ = link_available_;
  return true;
}

void DeviceSession::SetLinkAvailable(bool available) {
  link_available_ = available;
  if (!available) {
    current_link_identity_verified_ = false;
    current_identity_.reset();
    if (tracker_) {
      tracker_->InvalidateCurrentIdentity();
    }
  }
}

bool DeviceSession::CurrentLinkIdentityVerified() const {
  return current_link_identity_verified_;
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

bool DeviceSession::CanAcceptTelemetryMessage(bool is_basic_id) const {
  return is_basic_id || current_link_identity_verified_;
}

bool DeviceSession::CanSendDeviceCommands() const {
  const auto status = CurrentOnlineStatus();
  return link_available_ && current_link_identity_verified_ && status &&
         status->identity_status == IdentityStatus::kVerified;
}

bool DeviceSession::CanPublishTelemetry() const {
  const auto status = CurrentOnlineStatus();
  return CanSendDeviceCommands() && status &&
         status->business_status == BusinessStatus::kOnline;
}

}  // namespace runtime_status
