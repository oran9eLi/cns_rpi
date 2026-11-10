/** @file rtk_mavlink_fixture.cpp @brief 用官方 MAVLink 编码生成独立 F407/PX4 串口输入。 */
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <string>
#include <iomanip>
#include <cstdlib>
#include "common/mavlink.h"

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "decode-rtcm") {
    mavlink_message_t message{};
    mavlink_status_t status{};
    char byte;
    while (std::cin.get(byte)) {
      if (!mavlink_parse_char(MAVLINK_COMM_0, static_cast<std::uint8_t>(byte), &message, &status)) continue;
      std::cout << message.msgid;
      if (message.msgid == MAVLINK_MSG_ID_GPS_RTCM_DATA) {
        mavlink_gps_rtcm_data_t data{};
        mavlink_msg_gps_rtcm_data_decode(&message, &data);
        std::cout << " " << static_cast<unsigned>(data.flags) << " ";
        for (unsigned i = 0; i < data.len; ++i)
          std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(data.data[i]);
        std::cout << std::dec;
      }
      std::cout << "\n";
    }
    return 0;
  }
  if (argc == 3 && std::string_view(argv[1]) == "gps") {
    mavlink_gps_raw_int_t gps{};
    gps.time_usec = std::strtoull(argv[2], nullptr, 10);
    gps.fix_type = 6;
    gps.lat = 234567890;
    gps.lon = 1131234567;
    gps.alt = 12000;
    gps.satellites_visible = 19;
    gps.h_acc = 120;
    gps.v_acc = 240;
    mavlink_message_t message{};
    mavlink_msg_gps_raw_int_encode(1, MAV_COMP_ID_AUTOPILOT1, &message, &gps);
    std::uint8_t bytes[MAVLINK_MAX_PACKET_LEN]{};
    const auto length = mavlink_msg_to_send_buffer(bytes, &message);
    std::cout.write(reinterpret_cast<const char*>(bytes), length);
    return 0;
  }
  if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "box" &&
                   std::string_view(argv[1]) != "otherbox")) return 2;
  const bool box = argc == 2;
  const bool other_box = box && std::string_view(argv[1]) == "otherbox";
  const std::uint8_t system_id = box ? 42 : 1;
  // 193 是 F407 USART6 组件，必须与真实主程序的主控箱准入保持一致。
  const std::uint8_t component_id = box ? 193 : MAV_COMP_ID_AUTOPILOT1;
  const std::string_view device_id = other_box ? "DCDWCNS1RTKTEST00002"
                                             : box ? "DCDWCNS1RTKTEST00001" : "RTKINDOORPX4";
  mavlink_message_t heartbeat{}, identity{};
  mavlink_msg_heartbeat_pack(system_id, component_id, &heartbeat,
      box ? MAV_TYPE_ONBOARD_CONTROLLER : MAV_TYPE_QUADROTOR,
      box ? MAV_AUTOPILOT_INVALID : MAV_AUTOPILOT_PX4, 0, 0, MAV_STATE_STANDBY);
  std::uint8_t mac[20]{}, uas[20]{};
  std::memcpy(uas, device_id.data(), device_id.size());
  mavlink_msg_open_drone_id_basic_id_pack(system_id, component_id, &identity,
      0, 0, mac, MAV_ODID_ID_TYPE_SERIAL_NUMBER, MAV_ODID_UA_TYPE_HELICOPTER_OR_MULTIROTOR, uas);
  for (const auto& message : {heartbeat, identity}) {
    std::uint8_t bytes[MAVLINK_MAX_PACKET_LEN]{};
    const auto length = mavlink_msg_to_send_buffer(bytes, &message);
    std::cout.write(reinterpret_cast<const char*>(bytes), length);
  }
}
