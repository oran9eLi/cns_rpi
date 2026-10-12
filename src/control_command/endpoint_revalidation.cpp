/**
 * @file endpoint_revalidation.cpp
 * @brief F407 候选端点复核的状态实现。
 */

#include "control_command/endpoint_revalidation.hpp"

#include "protocol/identity.hpp"

namespace control_command {

HeartbeatObservation EndpointRevalidation::ObserveHeartbeat(
    const ControlledDeviceEndpoint& current,
    const ControlledDeviceEndpoint& observed, bool has_binding,
    Clock::time_point now) {
  if (!has_binding || current.type != device::Type::kCnsBox ||
      observed.type != device::Type::kCnsBox ||
      observed.endpoint.component_id != current.endpoint.component_id ||
      observed.endpoint.system_id == 0 || observed.endpoint.system_id > 250 ||
      observed.endpoint.system_id == current.endpoint.system_id) {
    return HeartbeatObservation::kUnchanged;
  }
  if (candidate_) {
    if (observed.endpoint.system_id == candidate_->system_id) {
      return HeartbeatObservation::kUnchanged;
    }
    ambiguous_ = true;
    return HeartbeatObservation::kAmbiguous;
  }
  candidate_ = observed.endpoint;
  started_at_ = now;
  warned_ = false;
  return HeartbeatObservation::kCandidate;
}

BasicIdObservation EndpointRevalidation::ObserveBasicId(
    const mavlink_message_t& message, std::string_view bound_device_id) {
  if (!candidate_ || ambiguous_ || conflict_ ||
      message.msgid != MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID ||
      message.sysid != candidate_->system_id ||
      message.compid != candidate_->component_id) {
    return BasicIdObservation::kIgnored;
  }
  mavlink_open_drone_id_basic_id_t basic_id{};
  mavlink_msg_open_drone_id_basic_id_decode(&message, &basic_id);
  const auto uas_id = protocol::ExtractUasId(basic_id.uas_id);
  if (!protocol::IsValidUasId(uas_id)) {
    return BasicIdObservation::kIgnored;
  }
  if (uas_id != bound_device_id) {
    conflict_ = true;
    return BasicIdObservation::kConflict;
  }
  return BasicIdObservation::kVerified;
}

bool EndpointRevalidation::Pending() const { return candidate_.has_value(); }
std::optional<MavlinkEndpoint> EndpointRevalidation::Candidate() const {
  return candidate_;
}
bool EndpointRevalidation::ShouldWarn(Clock::time_point now) {
  if (!candidate_ || warned_ || now - started_at_ < std::chrono::seconds(3)) {
    return false;
  }
  warned_ = true;
  return true;
}
void EndpointRevalidation::Reset() {
  candidate_.reset();
  ambiguous_ = false;
  conflict_ = false;
  warned_ = false;
}

}  // namespace control_command
