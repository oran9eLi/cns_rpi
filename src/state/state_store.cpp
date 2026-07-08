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

void StateStore::UpdateBatteryStatus(std::uint8_t id, const mavlink_battery_status_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (id < kBatteryCount) {
    state_.battery_status[id] = value;
  }
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

void StateStore::UpdateMotorPwmLow(std::uint8_t duty0, std::uint8_t duty1, bool run_state,
                                     std::uint8_t speed_level) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!state_.motor_pwm.has_value()) {
    state_.motor_pwm = MotorPwm{};
  }
  state_.motor_pwm->duty_percent[0] = duty0;
  state_.motor_pwm->duty_percent[1] = duty1;
  state_.motor_pwm->run_state = run_state;
  state_.motor_pwm->speed_level = speed_level;
}

void StateStore::UpdateMotorPwmHigh(std::uint8_t duty2, std::uint8_t duty3, bool run_state,
                                      std::uint8_t speed_level) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!state_.motor_pwm.has_value()) {
    state_.motor_pwm = MotorPwm{};
  }
  state_.motor_pwm->duty_percent[2] = duty2;
  state_.motor_pwm->duty_percent[3] = duty3;
  state_.motor_pwm->run_state = run_state;
  state_.motor_pwm->speed_level = speed_level;
}

void StateStore::UpdateGnssSat(const GnssSat& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.gnss_sat = value;
}

void StateStore::UpdateEnvHumidity(const EnvHumidity& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.env_humidity = value;
}

void StateStore::UpdateLoraStatus(const LoraStatus& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.lora_status = value;
}

void StateStore::UpdateRemoteIdStatus(const RemoteIdStatus& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.remote_id_status = value;
}

void StateStore::UpdateAlarmTable(const AlarmTable& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.alarm_table = value;
}

void StateStore::UpdateMessageLog(const MessageLog& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.message_log = value;
}

void StateStore::UpdateOpenDroneIdBasicId(const mavlink_open_drone_id_basic_id_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.open_drone_id_basic_id = value;
}

void StateStore::UpdateOpenDroneIdLocation(const mavlink_open_drone_id_location_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.open_drone_id_location = value;
}

void StateStore::UpdateOpenDroneIdSystem(const mavlink_open_drone_id_system_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.open_drone_id_system = value;
}

void StateStore::UpdateOpenDroneIdOperatorId(const mavlink_open_drone_id_operator_id_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.open_drone_id_operator_id = value;
}

void StateStore::UpdateOpenDroneIdSelfId(const mavlink_open_drone_id_self_id_t& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.open_drone_id_self_id = value;
}

void StateStore::UpdateVendorId(const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.vendor_id = value;
}

void StateStore::UpdateDcdwLabel(const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.dcdw_label = value;
}

void StateStore::UpdateRpiSerial(const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.rpi_serial = value;
}

TelemetryState StateStore::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

}  // namespace state
