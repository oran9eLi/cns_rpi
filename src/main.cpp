/**
 * @file main.cpp
 * @brief 程序入口，组合根。
 *
 * @details
 * M3a 阶段在 M2 双向收发闭环基础上接入遥测解码：
 * 收到帧 -> protocol::DecodeAndStore 写入 state::StateStore -> 打印解码后的
 * 有意义字段做人工验证。不接 MQTT（M5 的事），不处理扩展帧/身份帧（M3b/M3c 的事）。
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "common/mavlink.h"
#include "config/app_config.hpp"
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

  std::cout << "cns_rpi M3a 启动，串口=" << app_config->serial.device
            << " 波特率=" << app_config->serial.baud << std::endl;

  state::StateStore state_store;
  auto last_heartbeat = std::chrono::steady_clock::now();
  while (true) {
    if (auto msg = link->ReceiveMessage()) {
      if (protocol::DecodeAndStore(*msg, state_store)) {
        LogTelemetry(msg->msgid, state_store.Snapshot());
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
