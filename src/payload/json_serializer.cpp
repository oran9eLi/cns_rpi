/**
 * @file json_serializer.cpp
 * @brief json_serializer.hpp 的实现。
 */

#include "payload/json_serializer.hpp"

#include <numbers>

namespace payload {

namespace {

nlohmann::json BuildIdentity(const state::TelemetryState& state, const std::string& school_name) {
  nlohmann::json identity;
  if (state.vendor_id) {
    identity["vendor_id"] = *state.vendor_id;
  }
  if (state.dcdw_label) {
    identity["dcdw_label"] = *state.dcdw_label;
  }
  if (state.rpi_serial) {
    identity["rpi_serial"] = *state.rpi_serial;
  }
  identity["school_name"] = school_name;
  return identity;
}

constexpr double kRadToDeg = 180.0 / std::numbers::pi;

void AddHeartbeat(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.heartbeat) {
    return;
  }
  const auto& hb = *state.heartbeat;
  telemetry["heartbeat"] = {
      {"custom_mode", hb.custom_mode}, {"type", hb.type},
      {"autopilot", hb.autopilot},     {"base_mode", hb.base_mode},
      {"system_status", hb.system_status}, {"mavlink_version", hb.mavlink_version},
  };
}

void AddAttitude(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.attitude) {
    return;
  }
  const auto& att = *state.attitude;
  telemetry["attitude"] = {
      {"time_boot_ms", att.time_boot_ms},
      {"roll", att.roll * kRadToDeg},
      {"pitch", att.pitch * kRadToDeg},
      {"yaw", att.yaw * kRadToDeg},
      {"rollspeed", att.rollspeed * kRadToDeg},
      {"pitchspeed", att.pitchspeed * kRadToDeg},
      {"yawspeed", att.yawspeed * kRadToDeg},
  };
}

}  // namespace

nlohmann::json ToJson(const state::TelemetryState& state, const std::string& school_name) {
  nlohmann::json out;
  out["identity"] = BuildIdentity(state, school_name);

  nlohmann::json telemetry = nlohmann::json::object();
  AddHeartbeat(telemetry, state);
  AddAttitude(telemetry, state);
  if (!telemetry.empty()) {
    out["telemetry"] = std::move(telemetry);
  }

  return out;
}

}  // namespace payload
