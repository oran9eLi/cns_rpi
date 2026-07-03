/**
 * @file state_store.cpp
 * @brief state_store.hpp 的实现。
 */

#include "state/state_store.hpp"

namespace state {

void StateStore::UpdateHeartbeat(const mavlink_heartbeat_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.heartbeat = value;
}

void StateStore::UpdateGpsRawInt(const mavlink_gps_raw_int_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.gps_raw_int = value;
}

void StateStore::UpdateAttitude(const mavlink_attitude_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.attitude = value;
}

void StateStore::UpdateGlobalPositionInt(const mavlink_global_position_int_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.global_position_int = value;
}

void StateStore::UpdateSysStatus(const mavlink_sys_status_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.sys_status = value;
}

void StateStore::UpdateBatteryStatus(const mavlink_battery_status_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.battery_status = value;
}

void StateStore::UpdateScaledPressure(const mavlink_scaled_pressure_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.scaled_pressure = value;
}

TelemetryState StateStore::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

}  // namespace state
