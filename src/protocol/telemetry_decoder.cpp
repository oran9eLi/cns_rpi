/**
 * @file telemetry_decoder.cpp
 * @brief telemetry_decoder.hpp 的实现。
 */

#include "protocol/telemetry_decoder.hpp"

namespace protocol {

bool DecodeAndStore(const mavlink_message_t& msg, state::StateStore& store) {
  switch (msg.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT: {
      mavlink_heartbeat_t decoded{};
      mavlink_msg_heartbeat_decode(&msg, &decoded);
      store.UpdateHeartbeat(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
      mavlink_gps_raw_int_t decoded{};
      mavlink_msg_gps_raw_int_decode(&msg, &decoded);
      store.UpdateGpsRawInt(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_ATTITUDE: {
      mavlink_attitude_t decoded{};
      mavlink_msg_attitude_decode(&msg, &decoded);
      store.UpdateAttitude(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
      mavlink_global_position_int_t decoded{};
      mavlink_msg_global_position_int_decode(&msg, &decoded);
      store.UpdateGlobalPositionInt(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_SYS_STATUS: {
      mavlink_sys_status_t decoded{};
      mavlink_msg_sys_status_decode(&msg, &decoded);
      store.UpdateSysStatus(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_BATTERY_STATUS: {
      mavlink_battery_status_t decoded{};
      mavlink_msg_battery_status_decode(&msg, &decoded);
      store.UpdateBatteryStatus(decoded.id, decoded);
      return true;
    }
    case MAVLINK_MSG_ID_SCALED_PRESSURE: {
      mavlink_scaled_pressure_t decoded{};
      mavlink_msg_scaled_pressure_decode(&msg, &decoded);
      store.UpdateScaledPressure(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_SERVO_OUTPUT_RAW: {
      mavlink_servo_output_raw_t decoded{};
      mavlink_msg_servo_output_raw_decode(&msg, &decoded);
      state::MotorPulse pulse{};
      pulse.time_usec = decoded.time_usec;
      pulse.pwm_us = {decoded.servo1_raw, decoded.servo2_raw, decoded.servo3_raw,
                      decoded.servo4_raw};
      store.UpdateMotorPulse(pulse);
      return true;
    }
    default:
      return false;
  }
}

}  // namespace protocol
