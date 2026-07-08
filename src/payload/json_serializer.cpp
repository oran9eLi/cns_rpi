/**
 * @file json_serializer.cpp
 * @brief json_serializer.hpp 的实现。
 */

#include "payload/json_serializer.hpp"

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

}  // namespace

nlohmann::json ToJson(const state::TelemetryState& state, const std::string& school_name) {
  nlohmann::json out;
  out["identity"] = BuildIdentity(state, school_name);
  return out;
}

}  // namespace payload
