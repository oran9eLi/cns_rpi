/**
 * @file main.cpp
 * @brief 程序入口，组合根。
 *
 * @details
 * M3a 阶段在 M2 双向收发闭环基础上接入遥测解码，M3b 阶段接入扩展帧解码，
 * M3c 阶段接入身份帧解码：收到帧先更新 DCDW 角色号(帧头 sysid)，再依次尝试
 * protocol::DecodeAndStore（标准遥测）和 protocol::DecodeExtensionAndStore
 * （NAMED_VALUE_INT/TUNNEL 扩展帧 + OPEN_DRONE_ID_* 身份帧），
 * 写入 state::StateStore -> 打印解码后的有意义字段做人工验证。
 * 启动时读一次 RPi 本机序列号(V1 过渡期权威键)。不接 MQTT（M5 的事）。
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "common/mavlink.h"
#include "config/app_config.hpp"
#include "protocol/extension_decoder.hpp"
#include "protocol/identity.hpp"
#include "protocol/telemetry_decoder.hpp"
#include "state/state_store.hpp"
#include "uart/mavlink_link.hpp"

namespace {

constexpr std::uint8_t kSystemId = 1;
constexpr std::uint8_t kComponentId = MAV_COMP_ID_ONBOARD_COMPUTER;
constexpr auto kHeartbeatInterval = std::chrono::seconds(1);

/// RPi 自己的 HEARTBEAT：它不是飞控，所以 autopilot=MAV_AUTOPILOT_INVALID。
mavlink_message_t BuildHeartbeat() {
  mavlink_message_t msg{};
  mavlink_msg_heartbeat_pack(kSystemId, kComponentId, &msg, MAV_TYPE_ONBOARD_CONTROLLER,
                              MAV_AUTOPILOT_INVALID, /*base_mode=*/0, /*custom_mode=*/0,
                              MAV_STATE_ACTIVE);
  return msg;
}

/// 按刚解出来的这条帧的 msgid，打印 state_store 里对应字段的最新值——
/// 只是给人看的调试日志，不是解码逻辑本身（解码逻辑在 protocol::DecodeAndStore 里）。
void LogTelemetry(std::uint32_t msgid, const state::TelemetryState& snapshot) {
  switch (msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT:
      if (snapshot.heartbeat) {
        std::cout << "HEARTBEAT: type=" << static_cast<int>(snapshot.heartbeat->type)
                  << " system_status=" << static_cast<int>(snapshot.heartbeat->system_status)
                  << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_GPS_RAW_INT:
      if (snapshot.gps_raw_int) {
        std::cout << "GPS_RAW_INT: fix_type=" << static_cast<int>(snapshot.gps_raw_int->fix_type)
                  << " lat=" << snapshot.gps_raw_int->lat
                  << " lon=" << snapshot.gps_raw_int->lon << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_ATTITUDE:
      if (snapshot.attitude) {
        std::cout << "ATTITUDE: roll=" << snapshot.attitude->roll
                  << " pitch=" << snapshot.attitude->pitch
                  << " yaw=" << snapshot.attitude->yaw << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
      if (snapshot.global_position_int) {
        std::cout << "GLOBAL_POSITION_INT: lat=" << snapshot.global_position_int->lat
                  << " lon=" << snapshot.global_position_int->lon
                  << " relative_alt=" << snapshot.global_position_int->relative_alt << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_SYS_STATUS:
      if (snapshot.sys_status) {
        std::cout << "SYS_STATUS: voltage_battery=" << snapshot.sys_status->voltage_battery
                  << " battery_remaining="
                  << static_cast<int>(snapshot.sys_status->battery_remaining) << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_BATTERY_STATUS:
      if (snapshot.battery_status) {
        std::cout << "BATTERY_STATUS: voltages[0]=" << snapshot.battery_status->voltages[0]
                  << " battery_remaining="
                  << static_cast<int>(snapshot.battery_status->battery_remaining) << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_SCALED_PRESSURE:
      if (snapshot.scaled_pressure) {
        std::cout << "SCALED_PRESSURE: press_abs=" << snapshot.scaled_pressure->press_abs
                  << " temperature=" << snapshot.scaled_pressure->temperature << std::endl;
      }
      break;
    default:
      break;
  }
}

/// 跟 LogTelemetry 同样的定位：按扩展帧(M3b)/身份帧(M3c)的 msgid/内部语义
/// 打印 state_store 里对应字段的最新值，供真机人工验证；解码逻辑本身在
/// protocol::DecodeExtensionAndStore 里，这里只打印。
void LogExtension(std::uint32_t msgid, const state::TelemetryState& snapshot) {
  switch (msgid) {
    case MAVLINK_MSG_ID_NAMED_VALUE_INT:
      if (snapshot.module_status) {
        std::cout << "MODSTAT: [0]=" << static_cast<int>((*snapshot.module_status)[0])
                  << " [13]=" << static_cast<int>((*snapshot.module_status)[13]) << std::endl;
      }
      if (snapshot.battery2_status) {
        std::cout << "BAT2STAT: voltage_mv=" << snapshot.battery2_status->voltage_mv
                  << " percent=" << static_cast<int>(snapshot.battery2_status->percent)
                  << " low_voltage=" << snapshot.battery2_status->low_voltage << std::endl;
      }
      if (snapshot.motor_pwm) {
        std::cout << "MOTOR: duty=[" << static_cast<int>(snapshot.motor_pwm->duty_percent[0])
                  << "," << static_cast<int>(snapshot.motor_pwm->duty_percent[1]) << ","
                  << static_cast<int>(snapshot.motor_pwm->duty_percent[2]) << ","
                  << static_cast<int>(snapshot.motor_pwm->duty_percent[3])
                  << "] run_state=" << snapshot.motor_pwm->run_state
                  << " speed_level=" << static_cast<int>(snapshot.motor_pwm->speed_level)
                  << std::endl;
      }
      if (snapshot.gnss_sat) {
        std::cout << "GNSS_SAT: gps_visible=" << static_cast<int>(snapshot.gnss_sat->gps_visible)
                  << " gps_used=" << static_cast<int>(snapshot.gnss_sat->gps_used) << std::endl;
      }
      if (snapshot.env_humidity) {
        std::cout << "HUMIDITY: relative_humidity_x10=" << snapshot.env_humidity->relative_humidity_x10
                  << std::endl;
      }
      if (snapshot.lora_status) {
        std::cout << "LORASTAT: loss_rate_x10=" << snapshot.lora_status->loss_rate_x10
                  << " node_id=" << static_cast<int>(snapshot.lora_status->node_id)
                  << " present=" << snapshot.lora_status->present
                  << " link_state=" << static_cast<int>(snapshot.lora_status->link_state)
                  << std::endl;
      }
      if (snapshot.remote_id_status) {
        std::cout << "RIDSTAT: location_count=" << snapshot.remote_id_status->location_count
                  << " error_count=" << snapshot.remote_id_status->error_count
                  << " last_success_ms=" << snapshot.remote_id_status->last_success_ms
                  << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_TUNNEL:
      if (snapshot.alarm_table) {
        std::cout << "ALARM_TABLE: active_count=" << snapshot.alarm_table->active_count
                  << std::endl;
      }
      if (snapshot.message_log) {
        std::cout << "MESSAGE_LOG: latest_seq=" << snapshot.message_log->latest_seq
                  << " count=" << snapshot.message_log->count << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID:
      if (snapshot.open_drone_id_basic_id) {
        std::cout << "OPEN_DRONE_ID_BASIC_ID: id_type="
                  << static_cast<int>(snapshot.open_drone_id_basic_id->id_type)
                  << " ua_type=" << static_cast<int>(snapshot.open_drone_id_basic_id->ua_type)
                  << std::endl;
      }
      if (snapshot.vendor_id) {
        std::cout << "vendor_id=" << *snapshot.vendor_id << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_LOCATION:
      if (snapshot.open_drone_id_location) {
        std::cout << "OPEN_DRONE_ID_LOCATION: lat=" << snapshot.open_drone_id_location->latitude
                  << " lon=" << snapshot.open_drone_id_location->longitude << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SYSTEM:
      if (snapshot.open_drone_id_system) {
        std::cout << "OPEN_DRONE_ID_SYSTEM: operator_lat="
                  << snapshot.open_drone_id_system->operator_latitude << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_OPERATOR_ID:
      if (snapshot.open_drone_id_operator_id) {
        std::cout << "OPEN_DRONE_ID_OPERATOR_ID: operator_id_type="
                  << static_cast<int>(snapshot.open_drone_id_operator_id->operator_id_type)
                  << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SELF_ID:
      if (snapshot.open_drone_id_self_id) {
        std::cout << "OPEN_DRONE_ID_SELF_ID: description_type="
                  << static_cast<int>(snapshot.open_drone_id_self_id->description_type)
                  << std::endl;
      }
      break;
    default:
      break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string config_path = argc > 1 ? argv[1] : "config/config.json";

  auto app_config = config::LoadAppConfig(config_path);
  if (!app_config) {
    std::cerr << "读取配置失败: " << config_path << "\n";
    return EXIT_FAILURE;
  }

  auto link = uart::MavlinkLink::Open(app_config->serial.device, app_config->serial.baud);
  if (!link) {
    std::cerr << "打开串口失败: " << app_config->serial.device << "\n";
    return EXIT_FAILURE;
  }

  std::cout << "cns_rpi M3c 启动，串口=" << app_config->serial.device
            << " 波特率=" << app_config->serial.baud << std::endl;

  state::StateStore state_store;
  if (auto serial = protocol::ReadRpiSerial()) {
    state_store.UpdateRpiSerial(*serial);
  }
  auto last_heartbeat = std::chrono::steady_clock::now();
  while (true) {
    if (auto msg = link->ReceiveMessage()) {
      state_store.UpdateDcdwLabel(protocol::FormatDcdwLabel(msg->sysid));
      if (protocol::DecodeAndStore(*msg, state_store)) {
        LogTelemetry(msg->msgid, state_store.Snapshot());
      } else if (protocol::DecodeExtensionAndStore(*msg, state_store)) {
        LogExtension(msg->msgid, state_store.Snapshot());
      }
    }

    auto now = std::chrono::steady_clock::now();
    if (now - last_heartbeat >= kHeartbeatInterval) {
      mavlink_message_t heartbeat = BuildHeartbeat();
      link->SendMessage(heartbeat);
      last_heartbeat = now;
    }
  }
}
