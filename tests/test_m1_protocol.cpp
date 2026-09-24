#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include "common/mavlink.h"
#include "experiment/m1_protocol.hpp"

namespace {
constexpr std::uint64_t kNonce = 0x0102030405060708ULL;
constexpr std::uint8_t kSystem = 42;
constexpr std::uint8_t kPiComponent = 191;
constexpr std::uint8_t kF407Component = 193;
constexpr std::array<std::uint8_t, 14> kRequest{
    0x01, 0x03, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x03, 0x02, 0x01, 0x00, 0xE1, 0x00, 0x00};
constexpr std::array<std::uint8_t, 14> kAccepted{
    0x01, 0x04, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x03, 0x02, 0x01, 0x00, 0xE1, 0x00, 0x00};

mavlink_message_t Response(std::array<std::uint8_t, 14> data = kAccepted,
                           std::uint8_t length = 14,
                           std::uint16_t payload_type = 0x8003,
                           std::uint8_t source_component = kF407Component,
                           std::uint8_t target_component = kPiComponent) {
  std::array<std::uint8_t, MAVLINK_MSG_TUNNEL_FIELD_PAYLOAD_LEN> payload{};
  std::copy(data.begin(), data.end(), payload.begin());
  mavlink_message_t message{};
  mavlink_msg_tunnel_pack(kSystem, source_component, &message,
                          kSystem, target_component, payload_type, length,
                          payload.data());
  return message;
}

auto Decode(const mavlink_message_t& message) {
  return experiment::DecodeBaudChangeAccepted(
      message, kSystem, kF407Component, kSystem, kPiComponent, kNonce, 57600);
}
}  // namespace

TEST_CASE("类型3请求严格等于双方约定的14字节小端向量") {
  const auto frame = experiment::EncodeBaudChangeRequest(
      kNonce, 57600, kSystem, kPiComponent, kSystem, kF407Component);
  REQUIRE(frame.has_value());
  CHECK(static_cast<std::uint32_t>(frame->msgid) == MAVLINK_MSG_ID_TUNNEL);
  mavlink_tunnel_t tunnel{};
  mavlink_msg_tunnel_decode(&*frame, &tunnel);
  CHECK(tunnel.payload_type == 0x8003);
  CHECK(tunnel.payload_length == 14);
  CHECK(tunnel.target_system == kSystem);
  CHECK(tunnel.target_component == kF407Component);
  CHECK(std::equal(kRequest.begin(), kRequest.end(), tunnel.payload));
  CHECK_FALSE(experiment::EncodeBaudChangeRequest(
                  kNonce, 230400, kSystem, kPiComponent, kSystem, kF407Component)
                  .has_value());
}

TEST_CASE("类型4仅接受本轮目标、随机数和速率一致的旧速率回执") {
  const auto accepted = Decode(Response());
  REQUIRE(accepted.has_value());
  CHECK(accepted->nonce == kNonce);
  CHECK(accepted->accepted_baud_rate == 57600);
  CHECK_FALSE(Decode(Response(kAccepted, 14, 0x8002)).has_value());
  CHECK_FALSE(Decode(Response(kAccepted, 14, 0x8003, 194)).has_value());
  CHECK_FALSE(Decode(Response(kAccepted, 14, 0x8003, kF407Component, 192)).has_value());
}

TEST_CASE("类型4拒绝错误版本类型长度随机数和波特率") {
  auto data = kAccepted; data[0] = 2;
  CHECK_FALSE(Decode(Response(data)).has_value());
  data = kAccepted; data[1] = 2;
  CHECK_FALSE(Decode(Response(data)).has_value());
  CHECK_FALSE(Decode(Response(kAccepted, 13)).has_value());
  CHECK_FALSE(Decode(Response(kAccepted, 15)).has_value());
  data = kAccepted; data[2] ^= 1;
  CHECK_FALSE(Decode(Response(data)).has_value());
  data = kAccepted; data[11] = 0xC2; data[12] = 1;
  CHECK_FALSE(Decode(Response(data)).has_value());
}
