#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "common/mavlink.h"

namespace {

constexpr std::uint8_t kSystem = 42;
constexpr std::uint8_t kComponent = 193;
constexpr std::string_view kBoxId = "DCDWCNS1GNSSTEST0001";
using Payload = std::array<std::uint8_t, 128>;

void Send(const mavlink_message_t& message) {
  std::uint8_t bytes[MAVLINK_MAX_PACKET_LEN]{};
  const auto size = mavlink_msg_to_send_buffer(bytes, &message);
  std::cout.write(reinterpret_cast<const char*>(bytes), size);
}

void Put16(Payload& payload, std::size_t offset, std::uint16_t value) {
  payload[offset] = static_cast<std::uint8_t>(value);
  payload[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void Put32(Payload& payload, std::size_t offset, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    payload[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

void Raw(std::span<const std::uint8_t> wire, std::uint16_t crc,
         std::uint8_t protocol, std::uint8_t flags, std::uint32_t frame_id,
         std::uint32_t uptime) {
  const auto count = (wire.size() + 39) / 40;
  // Last-to-first order exercises the actual gateway assembler.
  for (std::size_t remaining = count; remaining > 0; --remaining) {
    const auto index = remaining - 1;
    const auto length = std::min<std::size_t>(40, wire.size() - index * 40);
    Payload payload{};
    payload[0] = 1;
    payload[1] = protocol;
    payload[2] = flags;
    payload[3] = 24;
    Put32(payload, 4, frame_id);
    Put32(payload, 8, uptime);
    Put16(payload, 12, static_cast<std::uint16_t>(wire.size()));
    Put16(payload, 14, static_cast<std::uint16_t>(index * 40));
    payload[16] = static_cast<std::uint8_t>(index);
    payload[17] = static_cast<std::uint8_t>(count);
    payload[18] = static_cast<std::uint8_t>(length);
    payload[19] = protocol == 1 ? 1 : 0;
    payload[20] = protocol == 2 ? 1 : 0;
    payload[21] = protocol == 2 ? 7 : 0;
    Put16(payload, 22, crc);
    std::copy_n(wire.begin() + index * 40, length, payload.begin() + 24);
    mavlink_message_t message{};
    mavlink_msg_tunnel_pack(kSystem, kComponent, &message, 0, 0, 0x8020,
                            static_cast<std::uint8_t>(24 + length), payload.data());
    Send(message);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const auto tick = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));
  mavlink_message_t message{};
  mavlink_msg_heartbeat_pack(kSystem, kComponent, &message,
      MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_STANDBY);
  Send(message);
  std::uint8_t mac[20]{}, uas[20]{};
  std::memcpy(uas, kBoxId.data(), kBoxId.size());
  mavlink_msg_open_drone_id_basic_id_pack(kSystem, kComponent, &message,
      0, 0, mac, MAV_ODID_ID_TYPE_SERIAL_NUMBER,
      MAV_ODID_UA_TYPE_HELICOPTER_OR_MULTIROTOR, uas);
  Send(message);

  mavlink_gps_raw_int_t gps{};
  gps.time_usec = static_cast<std::uint64_t>(tick) * 200000;
  gps.lat = 312300000 + static_cast<std::int32_t>(tick);
  gps.lon = 1214700000 + static_cast<std::int32_t>(tick);
  gps.alt = 12000;
  gps.fix_type = 3;
  gps.satellites_visible = 12;
  mavlink_msg_gps_raw_int_encode(kSystem, kComponent, &message, &gps);
  Send(message);

  constexpr std::string_view good =
      "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
  constexpr std::string_view bad =
      "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00\r\n";
  const auto bytes = [](std::string_view text) {
    return std::span{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
  };
  Raw(bytes(good), 0xBB87, 1, 3, tick * 3, tick * 200);
  Raw(bytes(bad), 0xF4E6, 1, 2, tick * 3 + 1, tick * 200);
  constexpr std::array<std::uint8_t, 10> ubx{0xB5,0x62,1,7,2,0,217,29,0,0};
  Raw(ubx, 0xDB35, 2, 3, tick * 3 + 2, tick * 200);
}
