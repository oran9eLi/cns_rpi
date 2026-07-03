/**
 * @file main.cpp
 * @brief 程序入口，组合根。
 *
 * @details
 * M2 阶段接入 UART/MAVLink 收发帧层的最小闭环：
 * 读配置 -> 打开 mavlink_link -> 循环收帧打日志 + 周期发送本机 HEARTBEAT。
 * 不解析帧内容（M3 的事），不接 MQTT（M5 的事）。
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "common/mavlink.h"
#include "config/app_config.hpp"
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

  std::cout << "cns_rpi M2 启动，串口=" << app_config->serial.device
            << " 波特率=" << app_config->serial.baud << std::endl;

  auto last_heartbeat = std::chrono::steady_clock::now();
  while (true) {
    if (auto msg = link->ReceiveMessage()) {
      std::cout << "收到帧 msgid=" << static_cast<int>(msg->msgid)
                << " len=" << static_cast<int>(msg->len)
                << " sysid=" << static_cast<int>(msg->sysid) << std::endl;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - last_heartbeat >= kHeartbeatInterval) {
      mavlink_message_t heartbeat = BuildHeartbeat();
      link->SendMessage(heartbeat);
      last_heartbeat = now;
    }
  }
}
