/**
 * @file px4_session.cpp
 * @brief RTK 实验 PX4 会话的身份核验、超时与消息调度。
 */

#include "rtk/px4_session.hpp"

#include "protocol/identity.hpp"
#include "protocol/telemetry_decoder.hpp"

namespace rtk {

Px4Session::Px4Session(std::uint8_t sender_system_id,
                       std::uint8_t sender_component_id,
                       std::chrono::milliseconds heartbeat_timeout)
    : sender_system_id_(sender_system_id),
      sender_component_id_(sender_component_id),
      heartbeat_timeout_(heartbeat_timeout) {}

bool Px4Session::Observe(const mavlink_message_t& message, Clock::time_point now,
                         std::span<const std::uint8_t> wire) {
  Tick(now);
  if (message.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
    mavlink_heartbeat_t heartbeat{};
    mavlink_msg_heartbeat_decode(&message, &heartbeat);
    if (message.sysid == 0 || message.compid != MAV_COMP_ID_AUTOPILOT1 ||
        heartbeat.autopilot != MAV_AUTOPILOT_PX4 ||
        (endpoint_ && message.sysid != endpoint_->system_id)) {
      return false;
    }
    endpoint_ = Px4Endpoint{message.sysid, message.compid};
    last_heartbeat_ = now;
    last_frame_ = now;
    store_.UpdateHeartbeat(heartbeat);
    return true;
  }
  if (!endpoint_ || !last_heartbeat_ ||
      message.sysid != endpoint_->system_id) {
    return false;
  }
  if (message.msgid == MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID) {
    mavlink_open_drone_id_basic_id_t basic_id{};
    mavlink_msg_open_drone_id_basic_id_decode(&message, &basic_id);
    const auto identity = protocol::ExtractUasId(basic_id.uas_id);
    if (!protocol::IsValidUasId(identity)) return false;
    if (bound_device_id_ && *bound_device_id_ != identity) {
      Invalidate();
      return false;
    }
    bound_device_id_ = identity;
    store_.UpdateControlledDevice(device::Type::kFlightController,
                                  endpoint_->system_id, endpoint_->component_id);
    (void)store_.UpdateDeviceId(identity);
    store_.UpdateOpenDroneIdBasicId(basic_id);
    last_frame_ = now;
    return true;
  }
  if (!Identity() || message.compid != endpoint_->component_id) return false;
  if (message.msgid == MAVLINK_MSG_ID_AUTOPILOT_VERSION) {
    mavlink_autopilot_version_t version{};
    mavlink_msg_autopilot_version_decode(&message, &version);
    firmware_ = version;
  } else if (message.msgid == MAVLINK_MSG_ID_GPS_RAW_INT) {
    ObserveGps(message, now, wire);
  } else if (!protocol::DecodeAndStore(message, store_)) return false;
  last_frame_ = now;
  return true;
}

void Px4Session::Tick(Clock::time_point now) {
  if (last_heartbeat_ && now - *last_heartbeat_ >= heartbeat_timeout_) {
    Invalidate();
  }
}

void Px4Session::Invalidate() {
  store_.ResetDeviceState();
  last_heartbeat_.reset();
  last_frame_.reset();
  last_identity_request_.reset();
  gps_.reset();
  gps_sequence_ = 0;
  gps_raw_hex_.clear();
  transitions_.clear();
  transitions_evicted_ = 0;
  firmware_.reset();
  observation_requested_ = false;
}

std::optional<std::string> Px4Session::Identity() const {
  return store_.Snapshot().device_id;
}

state::TelemetryState Px4Session::Snapshot() const { return store_.Snapshot(); }

std::optional<Px4Endpoint> Px4Session::Endpoint() const { return endpoint_; }

bool Px4Session::Online(Clock::time_point now) const {
  return last_heartbeat_ && now - *last_heartbeat_ < heartbeat_timeout_ && Identity();
}

std::optional<std::chrono::milliseconds> Px4Session::LastFrameAge(
    Clock::time_point now) const {
  if (!last_frame_) return std::nullopt;
  return std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_frame_);
}

std::vector<mavlink_message_t> Px4Session::PollOutbound(Clock::time_point now) {
  Tick(now);
  std::vector<mavlink_message_t> messages;
  if (!last_gcs_heartbeat_ || now - *last_gcs_heartbeat_ >= std::chrono::seconds(1)) {
    mavlink_message_t heartbeat{};
    mavlink_msg_heartbeat_pack(sender_system_id_, sender_component_id_, &heartbeat,
                               MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID, 0, 0,
                               MAV_STATE_ACTIVE);
    messages.push_back(heartbeat);
    last_gcs_heartbeat_ = now;
  }
  if (endpoint_ && last_heartbeat_ && !Identity() &&
      (!last_identity_request_ ||
       now - *last_identity_request_ >= std::chrono::seconds(2))) {
    mavlink_message_t request{};
    mavlink_msg_command_long_pack(
        sender_system_id_, sender_component_id_, &request,
        endpoint_->system_id, endpoint_->component_id, MAV_CMD_REQUEST_MESSAGE,
        0, MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID, 0, 0, 0, 0, 0, 0);
    messages.push_back(request);
    last_identity_request_ = now;
  }
  if (Online(now) && !observation_requested_) {
    mavlink_message_t interval{}, version{};
    mavlink_msg_command_long_pack(sender_system_id_, sender_component_id_, &interval,
        endpoint_->system_id, endpoint_->component_id, MAV_CMD_SET_MESSAGE_INTERVAL,
        0, MAVLINK_MSG_ID_GPS_RAW_INT, 1000000, 0, 0, 0, 0, 0);
    mavlink_msg_command_long_pack(sender_system_id_, sender_component_id_, &version,
        endpoint_->system_id, endpoint_->component_id, MAV_CMD_REQUEST_MESSAGE,
        0, MAVLINK_MSG_ID_AUTOPILOT_VERSION, 0, 0, 0, 0, 0, 0);
    messages.push_back(interval);
    messages.push_back(version);
    observation_requested_ = true;
  }
  return messages;
}

void Px4Session::ObserveGps(const mavlink_message_t& message, Clock::time_point now,
                            std::span<const std::uint8_t> wire) {
  mavlink_gps_raw_int_t sample{};
  mavlink_msg_gps_raw_int_decode(&message, &sample);
  gps_frame_time_ = now;
  if (!gps_ || sample.time_usec == 0 || sample.time_usec != gps_->time_usec) {
    ++gps_sequence_;
    gps_sample_time_ = now;
    if (!gps_ || sample.fix_type != gps_->fix_type) {
      if (transitions_.size() == 16) { transitions_.pop_front(); ++transitions_evicted_; }
      transitions_.push_back({gps_sequence_, sample.fix_type, now});
    }
    gps_ = sample;
    store_.UpdateGpsRawInt(sample);
    gps_raw_hex_.clear();
    constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : wire) {
      gps_raw_hex_ += digits[byte >> 4];
      gps_raw_hex_ += digits[byte & 15];
    }
  }
}

nlohmann::json Px4Session::GpsObservation(Clock::time_point now,
    std::optional<Clock::time_point> after,
    std::optional<Clock::time_point> through) const {
  using nlohmann::json;
  const auto age = [now](Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - time).count();
  };
  const bool stale = !gps_ || age(gps_sample_time_) >= 3000;
  json result{{"known", gps_.has_value()}, {"stale", stale},
      {"sample_sequence", gps_sequence_}, {"frame_age_ms", nullptr},
      {"sample_age_ms", nullptr}, {"source_time_usec", nullptr},
      {"fix_type", nullptr}, {"last_fix_type", nullptr}, {"fix_name", "unknown"},
      {"satellites_visible", nullptr}, {"lat_deg", nullptr}, {"lon_deg", nullptr},
      {"alt_m", nullptr}, {"h_acc_m", nullptr}, {"v_acc_m", nullptr},
      {"accuracy_kind", "receiver_estimate"}, {"raw_hex", gps_raw_hex_},
      {"transitions", json::array()}, {"transitions_evicted", transitions_evicted_}};
  for (const auto& transition : transitions_) {
    if ((after && transition.time <= *after) ||
        (through && transition.time > *through)) continue;
    result["transitions"].push_back({{"sample_sequence", transition.sequence},
        {"fix_type", transition.fix}, {"age_ms", age(transition.time)}});
  }
  if (!gps_) return result;
  const auto& gps = *gps_;
  result["frame_age_ms"] = age(gps_frame_time_);
  result["sample_age_ms"] = age(gps_sample_time_);
  result["source_time_usec"] = gps.time_usec;
  result["last_fix_type"] = gps.fix_type;
  if (!stale) result["fix_type"] = gps.fix_type;
  static constexpr const char* names[] = {"no_gps", "no_fix", "2d", "3d", "dgps", "rtk_float", "rtk_fixed", "static", "ppp"};
  result["fix_name"] = stale ? "stale" : (gps.fix_type < 9 ? names[gps.fix_type] : "unknown");
  if (gps.satellites_visible != 255) result["satellites_visible"] = gps.satellites_visible;
  if (gps.fix_type >= 3 && !stale) {
    result["lat_deg"] = gps.lat / 1e7;
    result["lon_deg"] = gps.lon / 1e7;
    result["alt_m"] = gps.alt / 1000.0;
    if (gps.h_acc != 0 && gps.h_acc != UINT32_MAX) result["h_acc_m"] = gps.h_acc / 1000.0;
    if (gps.v_acc != 0 && gps.v_acc != UINT32_MAX) result["v_acc_m"] = gps.v_acc / 1000.0;
  }
  return result;
}

nlohmann::json Px4Session::FirmwareObservation() const {
  if (!firmware_) return nullptr;
  return {{"flight_sw_version", firmware_->flight_sw_version},
      {"middleware_sw_version", firmware_->middleware_sw_version},
      {"os_sw_version", firmware_->os_sw_version}, {"board_version", firmware_->board_version},
      {"vendor_id", firmware_->vendor_id}, {"product_id", firmware_->product_id}};
}

}  // namespace rtk
