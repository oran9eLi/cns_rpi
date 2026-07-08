/**
 * @file json_serializer.cpp
 * @brief json_serializer.hpp 的实现。
 */

#include "payload/json_serializer.hpp"

#include <cstdint>
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

void AddGps(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.gps_raw_int) {
    return;
  }
  const auto& gps = *state.gps_raw_int;
  nlohmann::json out;
  out["time_usec"] = gps.time_usec;
  out["lat"] = static_cast<double>(gps.lat) / 1e7;
  out["lon"] = static_cast<double>(gps.lon) / 1e7;
  out["alt"] = static_cast<double>(gps.alt) / 1000.0;
  out["alt_ellipsoid"] = static_cast<double>(gps.alt_ellipsoid) / 1000.0;
  out["eph"] = (gps.eph == UINT16_MAX) ? nlohmann::json(nullptr) : nlohmann::json(gps.eph);
  out["epv"] = (gps.epv == UINT16_MAX) ? nlohmann::json(nullptr) : nlohmann::json(gps.epv);
  out["fix_type"] = gps.fix_type;
  out["satellites_visible"] = gps.satellites_visible;
  out["h_acc"] = static_cast<double>(gps.h_acc) / 1000.0;
  out["v_acc"] = static_cast<double>(gps.v_acc) / 1000.0;
  telemetry["gps"] = std::move(out);
}

void AddGlobalPosition(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.global_position_int) {
    return;
  }
  const auto& pos = *state.global_position_int;
  telemetry["global_position"] = {
      {"time_boot_ms", pos.time_boot_ms},
      {"lat", static_cast<double>(pos.lat) / 1e7},
      {"lon", static_cast<double>(pos.lon) / 1e7},
      {"alt", static_cast<double>(pos.alt) / 1000.0},
      {"hdg", static_cast<double>(pos.hdg) / 100.0},
  };
}

}  // namespace

nlohmann::json ToJson(const state::TelemetryState& state, const std::string& school_name) {
  nlohmann::json out;
  out["identity"] = BuildIdentity(state, school_name);

  nlohmann::json telemetry = nlohmann::json::object();
  AddHeartbeat(telemetry, state);
  AddAttitude(telemetry, state);
  AddGps(telemetry, state);
  AddGlobalPosition(telemetry, state);
  if (!telemetry.empty()) {
    out["telemetry"] = std::move(telemetry);
  }

  return out;
}

}  // namespace payload
