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

void StateStore::UpdateModStatusLow(const std::array<std::uint8_t, 8>& modules0to7) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!state_.module_status.has_value()) {
    state_.module_status = std::array<std::uint8_t, kModuleCount>{};
  }
  for (std::size_t i = 0; i < modules0to7.size(); ++i) {
    (*state_.module_status)[i] = modules0to7[i];
  }
}

void StateStore::UpdateModStatusHigh(const std::array<std::uint8_t, 6>& modules8to13) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!state_.module_status.has_value()) {
    state_.module_status = std::array<std::uint8_t, kModuleCount>{};
  }
  for (std::size_t i = 0; i < modules8to13.size(); ++i) {
    (*state_.module_status)[8 + i] = modules8to13[i];
  }
}

void StateStore::UpdateBattery2Status(const Battery2Status& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.battery2_status = value;
}

void StateStore::UpdateMotorPwm(const MotorPwm& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.motor_pwm = value;
}

void StateStore::UpdateGnssSat(const GnssSat& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.gnss_sat = value;
}

void StateStore::UpdateEnvHumidity(const EnvHumidity& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.env_humidity = value;
}

void StateStore::UpdateAlarmTable(const AlarmTable& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.alarm_table = value;
}

void StateStore::UpdateMessageLog(const MessageLog& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.message_log = value;
}

TelemetryState StateStore::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

}  // namespace state
