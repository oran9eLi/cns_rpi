/**
 * @file json_serializer.cpp
 * @brief json_serializer.hpp 的实现。
 */

#include "payload/json_serializer.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string>
#include <string_view>

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

void AddSysStatus(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.sys_status) {
    return;
  }
  const auto& sys = *state.sys_status;
  nlohmann::json out;
  out["onboard_control_sensors_present"] = sys.onboard_control_sensors_present;
  out["onboard_control_sensors_enabled"] = sys.onboard_control_sensors_enabled;
  out["onboard_control_sensors_health"] = sys.onboard_control_sensors_health;
  out["load"] = static_cast<double>(sys.load) / 10.0;
  out["voltage_battery"] = static_cast<double>(sys.voltage_battery) / 1000.0;
  out["current_battery"] = (sys.current_battery == -1)
                                ? nlohmann::json(nullptr)
                                : nlohmann::json(static_cast<double>(sys.current_battery) / 100.0);
  out["drop_rate_comm"] = static_cast<double>(sys.drop_rate_comm) / 10.0;
  out["errors_comm"] = sys.errors_comm;
  out["errors_count1"] = sys.errors_count1;
  out["errors_count2"] = sys.errors_count2;
  out["errors_count3"] = sys.errors_count3;
  out["errors_count4"] = sys.errors_count4;
  out["battery_remaining"] = sys.battery_remaining;
  out["onboard_control_sensors_present_extended"] = sys.onboard_control_sensors_present_extended;
  out["onboard_control_sensors_enabled_extended"] = sys.onboard_control_sensors_enabled_extended;
  out["onboard_control_sensors_health_extended"] = sys.onboard_control_sensors_health_extended;
  telemetry["sys_status"] = std::move(out);
}

nlohmann::json BuildBatteryStatusJson(const mavlink_battery_status_t& bs) {
  nlohmann::json out;
  out["current_consumed"] = bs.current_consumed;
  out["energy_consumed"] = static_cast<double>(bs.energy_consumed) * 100.0;
  out["temperature"] = static_cast<double>(bs.temperature) / 100.0;

  nlohmann::json voltages = nlohmann::json::array();
  for (std::uint16_t v : bs.voltages) {
    voltages.push_back((v == UINT16_MAX) ? nlohmann::json(nullptr)
                                          : nlohmann::json(static_cast<double>(v) / 1000.0));
  }
  out["voltages"] = std::move(voltages);

  out["current_battery"] = (bs.current_battery == -1)
                                ? nlohmann::json(nullptr)
                                : nlohmann::json(static_cast<double>(bs.current_battery) / 100.0);
  out["id"] = bs.id;
  out["battery_function"] = bs.battery_function;
  out["type"] = bs.type;
  out["battery_remaining"] =
      (bs.battery_remaining == -1) ? nlohmann::json(nullptr) : nlohmann::json(bs.battery_remaining);
  out["time_remaining"] = bs.time_remaining;
  out["charge_state"] = bs.charge_state;

  nlohmann::json voltages_ext = nlohmann::json::array();
  for (std::uint16_t v : bs.voltages_ext) {
    voltages_ext.push_back((v == 0) ? nlohmann::json(nullptr)
                                     : nlohmann::json(static_cast<double>(v) / 1000.0));
  }
  out["voltages_ext"] = std::move(voltages_ext);

  out["mode"] = bs.mode;
  out["fault_bitmask"] = bs.fault_bitmask;
  return out;
}

void AddBattery(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (state.battery_status[0]) {
    telemetry["battery"] = BuildBatteryStatusJson(*state.battery_status[0]);
  }
}

void AddBattery2(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (state.battery_status[1]) {
    telemetry["battery2"] = BuildBatteryStatusJson(*state.battery_status[1]);
  }
}

void AddPressure(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.scaled_pressure) {
    return;
  }
  const auto& p = *state.scaled_pressure;
  telemetry["pressure"] = {
      {"time_boot_ms", p.time_boot_ms},
      {"press_abs", static_cast<double>(p.press_abs)},
      {"press_diff", static_cast<double>(p.press_diff)},
      {"temperature", static_cast<double>(p.temperature) / 100.0},
      {"temperature_press_diff", static_cast<double>(p.temperature_press_diff) / 100.0},
  };
}

constexpr std::array<std::string_view, 7> kModuleStateNames = {
    "UNINITIALIZED", "STARTING", "ONLINE", "DEGRADED", "OFFLINE", "FAILED", "DISABLED"};

std::string ModuleStateToString(std::uint8_t state) {
  if (state < kModuleStateNames.size()) {
    return std::string(kModuleStateNames[state]);
  }
  return "UNKNOWN(" + std::to_string(state) + ")";
}

void AddGnssSat(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.gnss_sat) {
    return;
  }
  const auto& s = *state.gnss_sat;
  telemetry["gnss_sat"] = {
      {"gps_visible", s.gps_visible},
      {"beidou_visible", s.beidou_visible},
      {"gps_used", s.gps_used},
      {"beidou_used", s.beidou_used},
  };
}

void AddHumidity(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.env_humidity) {
    return;
  }
  telemetry["humidity"]["humidity_percent"] =
      static_cast<double>(state.env_humidity->relative_humidity_x10) / 10.0;
}

void AddMotor(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.motor_pwm) {
    return;
  }
  const auto& m = *state.motor_pwm;
  telemetry["motor"] = {
      {"duty_percent", m.duty_percent},
      {"run_state", m.run_state},
      {"speed_level", m.speed_level},
  };
}

void AddLora(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.lora_status) {
    return;
  }
  const auto& l = *state.lora_status;
  telemetry["lora"] = {
      {"loss_rate_percent", static_cast<double>(l.loss_rate_x10) / 10.0},
      {"node_id", l.node_id},
      {"present", l.present},
      {"link_state", ModuleStateToString(l.link_state)},
  };
}

void AddRemoteId(nlohmann::json& telemetry, const state::TelemetryState& state) {
  if (!state.remote_id_status) {
    return;
  }
  const auto& r = *state.remote_id_status;
  telemetry["remote_id"] = {
      {"location_count", r.location_count},
      {"error_count", r.error_count},
      {"last_success_ms", r.last_success_ms},
  };
}

constexpr std::array<std::string_view, 14> kModuleNames = {
    "GNSS",       "IMU",      "BARO",       "BATTERY", "LORA",    "5G",
    "STORAGE",    "REMOTE_ID", "DISPLAY",   "CONTROL", "ALARM",   "SYSTEM",
    "ESTIMATOR",  "BUSINESS"};

nlohmann::json BuildModules(const state::TelemetryState& state) {
  nlohmann::json modules = nlohmann::json::array();
  for (std::size_t i = 0; i < state.module_status->size(); ++i) {
    modules.push_back({
        {"name", kModuleNames[i]},
        {"status", ModuleStateToString((*state.module_status)[i])},
    });
  }
  return modules;
}

nlohmann::json BuildAlarms(const state::AlarmTable& table) {
  nlohmann::json entries = nlohmann::json::array();
  for (std::size_t i = 0; i < table.active_count; ++i) {
    const auto& e = table.entries[i];
    entries.push_back({
        {"source_id", e.source_id},
        {"fault_code", e.fault_code},
        {"severity", e.severity},
        {"active", e.active},
        {"age_s", e.age_s},
    });
  }
  return {{"ver", table.ver}, {"entries", std::move(entries)}};
}

std::string FormatTimeHhMmSs(const std::array<std::uint8_t, 3>& hms) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hms[0], hms[1], hms[2]);
  return std::string(buf);
}

nlohmann::json BuildLogs(const state::MessageLog& log) {
  nlohmann::json entries = nlohmann::json::array();
  for (std::size_t i = 0; i < log.count; ++i) {
    const auto& e = log.entries[i];
    entries.push_back({
        {"sequence", e.sequence},
        {"message_id", e.message_id},
        {"time", FormatTimeHhMmSs(e.time_hhmmss)},
        {"severity", e.severity},
    });
  }
  return {{"latest_seq", log.latest_seq}, {"entries", std::move(entries)}};
}

std::string ToTrimmedString(const char* data, std::size_t max_len) {
  return std::string(data, strnlen(data, max_len));
}

void AddDroneIdBasicId(nlohmann::json& drone_id, const state::TelemetryState& state) {
  if (!state.open_drone_id_basic_id) {
    return;
  }
  const auto& b = *state.open_drone_id_basic_id;
  drone_id["basic_id"] = {
      {"id_type", b.id_type},
      {"ua_type", b.ua_type},
      {"uas_id", ToTrimmedString(reinterpret_cast<const char*>(b.uas_id), sizeof(b.uas_id))},
  };
}

void AddDroneIdLocation(nlohmann::json& drone_id, const state::TelemetryState& state) {
  if (!state.open_drone_id_location) {
    return;
  }
  const auto& l = *state.open_drone_id_location;
  nlohmann::json out;
  out["latitude"] = static_cast<double>(l.latitude) / 1e7;
  out["longitude"] = static_cast<double>(l.longitude) / 1e7;
  out["altitude_barometric"] = (l.altitude_barometric == -1000.0F)
                                    ? nlohmann::json(nullptr)
                                    : nlohmann::json(static_cast<double>(l.altitude_barometric));
  out["altitude_geodetic"] = (l.altitude_geodetic == -1000.0F)
                                  ? nlohmann::json(nullptr)
                                  : nlohmann::json(static_cast<double>(l.altitude_geodetic));
  out["timestamp"] = static_cast<double>(l.timestamp);
  out["status"] = l.status;
  out["horizontal_accuracy"] = l.horizontal_accuracy;
  out["vertical_accuracy"] = l.vertical_accuracy;
  out["barometer_accuracy"] = l.barometer_accuracy;
  out["timestamp_accuracy"] = l.timestamp_accuracy;
  drone_id["location"] = std::move(out);
}

void AddDroneIdSystem(nlohmann::json& drone_id, const state::TelemetryState& state) {
  if (!state.open_drone_id_system) {
    return;
  }
  const auto& s = *state.open_drone_id_system;
  nlohmann::json out;
  out["operator_latitude"] = static_cast<double>(s.operator_latitude) / 1e7;
  out["operator_longitude"] = static_cast<double>(s.operator_longitude) / 1e7;
  out["operator_altitude_geo"] = (s.operator_altitude_geo == -1000.0F)
                                      ? nlohmann::json(nullptr)
                                      : nlohmann::json(static_cast<double>(s.operator_altitude_geo));
  out["timestamp"] = s.timestamp;
  out["operator_location_type"] = s.operator_location_type;
  out["classification_type"] = s.classification_type;
  out["category_eu"] = s.category_eu;
  out["class_eu"] = s.class_eu;
  drone_id["system"] = std::move(out);
}

void AddDroneIdOperatorId(nlohmann::json& drone_id, const state::TelemetryState& state) {
  if (!state.open_drone_id_operator_id) {
    return;
  }
  const auto& o = *state.open_drone_id_operator_id;
  drone_id["operator_id"] = {
      {"operator_id_type", o.operator_id_type},
      {"operator_id", ToTrimmedString(o.operator_id, sizeof(o.operator_id))},
  };
}

void AddDroneIdSelfId(nlohmann::json& drone_id, const state::TelemetryState& state) {
  if (!state.open_drone_id_self_id) {
    return;
  }
  const auto& s = *state.open_drone_id_self_id;
  drone_id["self_id"] = {
      {"description_type", s.description_type},
      {"description", ToTrimmedString(s.description, sizeof(s.description))},
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
  AddSysStatus(telemetry, state);
  AddBattery(telemetry, state);
  AddBattery2(telemetry, state);
  AddPressure(telemetry, state);
  AddGnssSat(telemetry, state);
  AddHumidity(telemetry, state);
  AddMotor(telemetry, state);
  AddLora(telemetry, state);
  AddRemoteId(telemetry, state);
  if (!telemetry.empty()) {
    out["telemetry"] = std::move(telemetry);
  }

  if (state.module_status) {
    out["modules"] = BuildModules(state);
  }

  if (state.alarm_table) {
    out["alarms"] = BuildAlarms(*state.alarm_table);
  }
  if (state.message_log) {
    out["logs"] = BuildLogs(*state.message_log);
  }

  nlohmann::json drone_id = nlohmann::json::object();
  AddDroneIdBasicId(drone_id, state);
  AddDroneIdLocation(drone_id, state);
  AddDroneIdSystem(drone_id, state);
  AddDroneIdOperatorId(drone_id, state);
  AddDroneIdSelfId(drone_id, state);
  if (!drone_id.empty()) {
    out["drone_id"] = std::move(drone_id);
  }

  return out;
}

}  // namespace payload
