#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/mavlink.h"
#include "control_command/control_command.hpp"
#include "uart/mavlink_link.hpp"

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  if (std::string{argv[1]} == "unverified") {
    mavlink_message_t message{};
    std::uint8_t bytes[MAVLINK_MAX_PACKET_LEN]{};
    mavlink_msg_heartbeat_pack(42, 193, &message, MAV_TYPE_ONBOARD_CONTROLLER,
        MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_STANDBY);
    auto size = mavlink_msg_to_send_buffer(bytes, &message);
    std::cout.write(reinterpret_cast<const char*>(bytes), size);
    std::uint8_t mac[20]{}, uas[20]{};
    constexpr auto identity = "DCDWCNS1GNSSTEST0001";
    std::memcpy(uas, identity, std::strlen(identity));
    mavlink_msg_open_drone_id_basic_id_pack(42, 194, &message, 0, 0, mac,
        MAV_ODID_ID_TYPE_SERIAL_NUMBER, MAV_ODID_UA_TYPE_HELICOPTER_OR_MULTIROTOR, uas);
    size = mavlink_msg_to_send_buffer(bytes, &message);
    std::cout.write(reinterpret_cast<const char*>(bytes), size);
    return 0;
  }
  if (std::string{argv[1]} != "decode") {
    mavlink_message_t message{};
    mavlink_msg_command_ack_pack(42, 193, &message,
        control_command::kSetMotorPwmUs,
        static_cast<std::uint8_t>(std::strtoul(argv[1], nullptr, 10)),
        50, 0, 42, MAV_COMP_ID_ONBOARD_COMPUTER);
    std::uint8_t bytes[MAVLINK_MAX_PACKET_LEN]{};
    const auto size = mavlink_msg_to_send_buffer(bytes, &message);
    std::cout.write(reinterpret_cast<const char*>(bytes), size);
    return 0;
  }
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{std::cin}, {}};
  uart::MavlinkFrameAssembler assembler;
  nlohmann::json results = nlohmann::json::array();
  auto message = assembler.Feed(bytes);
  while (message) {
    if (message->msgid == MAVLINK_MSG_ID_TUNNEL) {
      mavlink_tunnel_t tunnel{};
      mavlink_msg_tunnel_decode(&*message, &tunnel);
      if (tunnel.payload_type == 0x8040) {
        results.push_back({{"kind", "feedback"}, {"version", tunnel.payload[0]},
            {"result", tunnel.payload[1]}, {"length", tunnel.payload_length},
            {"system", message->sysid}, {"component", message->compid},
            {"target_system", tunnel.target_system},
            {"target_component", tunnel.target_component}});
      }
    } else if (message->msgid == MAVLINK_MSG_ID_COMMAND_LONG) {
      mavlink_command_long_t command{};
      mavlink_msg_command_long_decode(&*message, &command);
      results.push_back({{"kind", "control"}, {"command", command.command}});
    }
    message = assembler.Feed({});
  }
  std::cout << results.dump() << '\n';
}
