#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

#include "uart/mavlink_link.hpp"

namespace bmi_fixture {
using Bytes = std::array<std::uint8_t, 128>;
inline nlohmann::json Contract() {
  return nlohmann::json::parse(std::ifstream(SOURCE_DIR "/tests/fixtures/bmi088_contract.json"));
}
inline void Put32(Bytes& bytes, int offset, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) bytes[offset + i] = value >> (8 * i);
}
inline Bytes Payload(int kind) {
  Bytes result{};
  if (kind < 2) {
    const auto name = kind == 0 ? "raw_payload_hex" : "parsed_payload_hex";
    const auto hex = Contract()["pair"]["imu"][name].get<std::string>();
    for (std::size_t i = 0; i < 64; ++i)
      result[i] = std::stoul(hex.substr(i * 2, 2), nullptr, 16);
    return result;
  }
  result[0] = 1; result[1] = 2; result[2] = 3; result[3] = 1;
  Put32(result, 4, 42); Put32(result, 8, 10000); Put32(result, 12, 10010);
  Put32(result, 16, 1); Put32(result, 28, 42); Put32(result, 32, 39);
  Put32(result, 36, 3); Put32(result, 56, 10);
  result[51] = 1; result[52] = 0x1e; result[53] = 0x0f;
  result[54] = 0x18; result[55] = 0x68;
  result[60] = 1; result[61] = 1; result[62] = 2;
  return result;
}
inline mavlink_message_t Message(const Bytes& bytes, int kind, std::uint8_t sys = 218,
                                 std::uint8_t comp = 193, int length = 64,
                                 int target = 0) {
  mavlink_message_t packed{};
  mavlink_msg_tunnel_pack(sys, comp, &packed, target, 0, 0x8030 + kind, length, bytes.data());
  std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> wire{};
  const auto size = mavlink_msg_to_send_buffer(wire.data(), &packed);
  uart::MavlinkFrameAssembler assembler;
  auto verified = assembler.Feed(std::span(wire.data(), size));
  if (!verified) throw std::runtime_error("fixture CRC failed");
  return *verified;
}
}  // namespace bmi_fixture
