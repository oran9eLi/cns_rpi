/**
 * @file m1_protocol.cpp
 * @brief MAVLink 2 TUNNEL 内的 M1 改参帧编解码与字节边界校验。
 */

#include "experiment/m1_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace experiment {
namespace {
constexpr std::uint16_t kPayloadType = 0x8003;
constexpr std::uint8_t kVersion = 1;
constexpr std::uint8_t kRequestType = 3;
constexpr std::uint8_t kAcceptedType = 4;
constexpr std::size_t kDataBytes = 14;
constexpr std::size_t kTunnelHeaderBytes = 5;

bool AllowedBaud(std::uint32_t baud_rate) {
  return baud_rate == 57600 || baud_rate == 115200;
}

std::uint64_t ReadNonce(const std::uint8_t* bytes) {
  std::uint64_t nonce = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    nonce |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  return nonce;
}

std::uint32_t ReadBaud(const std::uint8_t* bytes) {
  std::uint32_t baud_rate = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    baud_rate |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
  }
  return baud_rate;
}
}  // namespace

std::expected<mavlink_message_t, std::string> EncodeBaudChangeRequest(
    std::uint64_t nonce, std::uint32_t baud_rate,
    std::uint8_t source_system, std::uint8_t source_component,
    std::uint8_t target_system, std::uint8_t target_component) {
  if (nonce == 0 || !AllowedBaud(baud_rate) || source_system == 0 ||
      source_component == 0 || target_system == 0 || target_component == 0) {
    return std::unexpected("改参请求端点、随机数或目标速率非法");
  }
  std::array<std::uint8_t, MAVLINK_MSG_TUNNEL_FIELD_PAYLOAD_LEN> data{};
  data[0] = kVersion;
  data[1] = kRequestType;
  for (std::size_t index = 0; index < 8; ++index) {
    data[2 + index] = static_cast<std::uint8_t>(nonce >> (index * 8));
  }
  for (std::size_t index = 0; index < 4; ++index) {
    data[10 + index] = static_cast<std::uint8_t>(baud_rate >> (index * 8));
  }
  mavlink_message_t message{};
  mavlink_msg_tunnel_pack(source_system, source_component, &message,
                          target_system, target_component, kPayloadType,
                          static_cast<std::uint8_t>(kDataBytes), data.data());
  return message;
}

std::expected<BaudChangeAcceptance, std::string> DecodeBaudChangeAccepted(
    const mavlink_message_t& message,
    std::uint8_t expected_source_system, std::uint8_t expected_source_component,
    std::uint8_t expected_target_system, std::uint8_t expected_target_component,
    std::uint64_t expected_nonce, std::uint32_t expected_baud_rate) {
  if (message.msgid != MAVLINK_MSG_ID_TUNNEL ||
      expected_source_system == 0 || expected_source_component == 0 ||
      expected_target_system == 0 || expected_target_component == 0 ||
      expected_nonce == 0 || !AllowedBaud(expected_baud_rate) ||
      message.sysid != expected_source_system ||
      message.compid != expected_source_component) {
    return std::unexpected("不是当前F407发来的改参接受帧");
  }
  mavlink_tunnel_t tunnel{};
  mavlink_msg_tunnel_decode(&message, &tunnel);
  // MAVLink 2 允许裁剪末尾零字节；声明的逻辑长度仍必须恰为 14。
  if (tunnel.payload_type != kPayloadType ||
      tunnel.target_system != expected_target_system ||
      tunnel.target_component != expected_target_component ||
      tunnel.payload_length != kDataBytes ||
      message.len < kTunnelHeaderBytes + 12 ||
      message.len > kTunnelHeaderBytes + kDataBytes ||
      tunnel.payload[0] != kVersion ||
      tunnel.payload[1] != kAcceptedType) {
    return std::unexpected("改参接受帧类型、目标或长度非法");
  }
  const auto nonce = ReadNonce(tunnel.payload + 2);
  const auto accepted_baud_rate = ReadBaud(tunnel.payload + 10);
  if (nonce != expected_nonce || accepted_baud_rate != expected_baud_rate) {
    return std::unexpected("改参接受帧随机数或速率与本次动作不符");
  }
  return BaudChangeAcceptance{nonce, accepted_baud_rate};
}

}  // namespace experiment
