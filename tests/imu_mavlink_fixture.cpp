#include <cstring>
#include <iostream>
#include <string>

#include "bmi088_fixture.hpp"

namespace {
void Send(const mavlink_message_t& message) {
  std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> bytes{};
  const auto length = mavlink_msg_to_send_buffer(bytes.data(), &message);
  std::cout.write(reinterpret_cast<const char*>(bytes.data()), length);
}
}

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  const auto tick = static_cast<std::uint32_t>(std::stoul(argv[1]));
  const std::string mode = argv[2];
  const auto sample_tick = static_cast<std::uint32_t>(std::stoul(argv[3]));
  mavlink_message_t message{};
  mavlink_msg_heartbeat_pack(218, 193, &message, MAV_TYPE_ONBOARD_CONTROLLER,
      MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_STANDBY);
  Send(message);
  if (mode != "noid") {
    std::uint8_t mac[20]{}, uas[20]{};
    constexpr char id[] = "DCDWCNS1S2MEAG1VTA0C";
    std::memcpy(uas, id, sizeof(id) - 1);
    mavlink_msg_open_drone_id_basic_id_pack(218, 193, &message, 0, 0, mac,
        MAV_ODID_ID_TYPE_SERIAL_NUMBER, MAV_ODID_UA_TYPE_NONE, uas);
    Send(message);
  }
  mavlink_msg_attitude_pack(218, 193, &message, tick * 200, 0.1f, 0.2f, 0.3f, 0, 0, 0);
  Send(message);
  if (mode == "basic") return 0;
  const bool measurement = mode == "samples" || mode == "frozen" || mode == "noid" ||
                           mode == "foreign" || mode == "mismatch";
  if (measurement) {
    for (int duplicate = 0; duplicate < 2; ++duplicate) {
      for (int kind : {1, 0}) {
        auto bytes = bmi_fixture::Payload(kind);
        bmi_fixture::Put32(bytes, 4, sample_tick);
        bmi_fixture::Put32(bytes, 8, 10000 + sample_tick * 200);
        bmi_fixture::Put32(bytes, 12, 10000 + sample_tick * 200 + (kind == 0 ? 5 : 8));
        if (mode == "mismatch" && kind == 1) bmi_fixture::Put32(bytes, 28, 1010);
        Send(bmi_fixture::Message(bytes, kind, mode == "foreign" ? 219 : 218));
      }
    }
  }
  if (mode == "status" || mode == "withdraw" || (mode == "samples" && tick % 5 == 0)) {
    auto bytes = bmi_fixture::Payload(2);
    bmi_fixture::Put32(bytes, 4, tick);
    bmi_fixture::Put32(bytes, 8, 10000 + tick * 200);
    bmi_fixture::Put32(bytes, 12, 10000 + tick * 200 + 10);
    if (mode == "withdraw") { bytes[61] = 0; bytes[2] = 9; }
    Send(bmi_fixture::Message(bytes, 2));
  }
}
