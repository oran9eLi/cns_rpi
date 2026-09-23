#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "common/mavlink.h"
#include "experiment/uart_echo_protocol.hpp"

namespace {

constexpr std::uint64_t kNonce = 0x0102030405060708ULL;
constexpr std::uint8_t kSystem = 42;
constexpr std::uint8_t kPiComponent = 191;
constexpr std::uint8_t kF407Component = 193;

mavlink_message_t Response(const std::vector<std::uint8_t>& text,
                           std::uint32_t baud = 57600,
                           std::uint16_t payload_type = 0x8003,
                           std::uint8_t target_component = kPiComponent,
                           bool extra_data = false) {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 1;
  payload[1] = 2;
  for (std::size_t index = 0; index < 8; ++index) {
    payload[2 + index] = static_cast<std::uint8_t>(kNonce >> (8 * index));
  }
  payload[10] = static_cast<std::uint8_t>(text.size());
  std::copy(text.begin(), text.end(), payload.begin() + 11);
  const auto offset = 11 + text.size();
  for (std::size_t index = 0; index < 4; ++index) {
    payload[offset + index] = static_cast<std::uint8_t>(baud >> (8 * index));
  }
  if (extra_data) payload[offset + 4] = 0x42;
  mavlink_message_t message{};
  mavlink_msg_tunnel_pack(kSystem, kF407Component, &message, kSystem,
                          target_component, payload_type,
                          static_cast<std::uint8_t>(offset + 4),
                          payload.data());
  return message;
}

}  // namespace

TEST_CASE("候选请求载荷按小端编码且保留学生混合文本") {
  const std::vector<std::uint8_t> text{0x41, 0xE4, 0xB8, 0xAD};
  const auto message = experiment::EncodeEchoRequest(
      text, kNonce, kSystem, kPiComponent, kSystem, kF407Component);
  REQUIRE(message.has_value());
  CHECK(static_cast<std::uint32_t>(message->msgid) == MAVLINK_MSG_ID_TUNNEL);
  mavlink_tunnel_t tunnel{};
  mavlink_msg_tunnel_decode(&*message, &tunnel);
  CHECK(tunnel.payload_type == 0x8003);
  CHECK(tunnel.target_system == kSystem);
  CHECK(tunnel.target_component == kF407Component);
  CHECK(tunnel.payload_length == 15);
  const std::array<std::uint8_t, 15> expected{
      1, 1, 8, 7, 6, 5, 4, 3, 2, 1, 4, 0x41, 0xE4, 0xB8, 0xAD};
  CHECK(std::equal(expected.begin(), expected.end(), tunnel.payload));
}

TEST_CASE("候选响应取得原样文本和合成波特率") {
  const auto decoded = experiment::DecodeEchoResponse(
      Response({0x41, 0xE4, 0xB8, 0xAD}), kSystem, kPiComponent);
  REQUIRE(decoded.has_value());
  CHECK(decoded->nonce == kNonce);
  CHECK(decoded->text_bytes == std::vector<std::uint8_t>{0x41, 0xE4, 0xB8, 0xAD});
  CHECK(decoded->f407_baud_rate == 57600);
}

TEST_CASE("错误类型目标和逻辑尾随数据均被拒绝") {
  CHECK_FALSE(experiment::DecodeEchoResponse(
                  Response({0x41}, 57600, 0x8002), kSystem, kPiComponent)
                  .has_value());
  CHECK_FALSE(experiment::DecodeEchoResponse(
                  Response({0x41}, 57600, 0x8003, 192), kSystem, kPiComponent)
                  .has_value());
  CHECK_FALSE(experiment::DecodeEchoResponse(
                  Response({0x41}, 57600, 0x8003, kPiComponent, true),
                  kSystem, kPiComponent)
                  .has_value());
}

TEST_CASE("实际帧十六进制为大写偶数位") {
  const std::array<std::uint8_t, 4> bytes{0xFD, 0x00, 0x0A, 0xBC};
  CHECK(experiment::FrameHex(bytes) == "FD000ABC");
}
