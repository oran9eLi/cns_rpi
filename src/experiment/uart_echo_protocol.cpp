/**
 * @file uart_echo_protocol.cpp
 * @brief 候选 H1 TUNNEL 载荷编解码；数值待三端实机冻结。
 */

#include "experiment/uart_echo_protocol.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace experiment {
namespace {

constexpr std::uint16_t kPayloadType = 0x8003;
constexpr std::uint8_t kVersion = 1;
constexpr std::uint8_t kRequestType = 1;
constexpr std::uint8_t kResponseType = 2;
constexpr std::size_t kTextOffset = 11;
constexpr std::size_t kMaximumTextBytes = 48;
constexpr std::size_t kBaudBytes = 4;
constexpr std::size_t kTunnelHeaderBytes = 5;

std::uint64_t ReadNonce(const std::uint8_t* data) {
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    result |= static_cast<std::uint64_t>(data[index]) << (index * 8);
  }
  return result;
}

std::uint32_t ReadBaud(const std::uint8_t* data) {
  std::uint32_t result = 0;
  for (std::size_t index = 0; index < kBaudBytes; ++index) {
    result |= static_cast<std::uint32_t>(data[index]) << (index * 8);
  }
  return result;
}

}  // namespace

std::expected<mavlink_message_t, std::string> EncodeEchoRequest(
    std::span<const std::uint8_t> text_bytes, std::uint64_t nonce,
    std::uint8_t source_system, std::uint8_t source_component,
    std::uint8_t target_system, std::uint8_t target_component) {
  if (text_bytes.empty() || text_bytes.size() > kMaximumTextBytes || nonce == 0 ||
      source_system == 0 || source_component == 0 ||
      target_system == 0 || target_component == 0) {
    return std::unexpected("回显请求端点、随机数或文本长度非法");
  }
  std::array<std::uint8_t, MAVLINK_MSG_TUNNEL_FIELD_PAYLOAD_LEN> payload{};
  payload[0] = kVersion;
  payload[1] = kRequestType;
  for (std::size_t index = 0; index < 8; ++index) {
    payload[2 + index] = static_cast<std::uint8_t>(nonce >> (index * 8));
  }
  payload[10] = static_cast<std::uint8_t>(text_bytes.size());
  std::copy(text_bytes.begin(), text_bytes.end(), payload.begin() + kTextOffset);
  mavlink_message_t message{};
  mavlink_msg_tunnel_pack(
      source_system, source_component, &message, target_system,
      target_component, kPayloadType,
      static_cast<std::uint8_t>(kTextOffset + text_bytes.size()),
      payload.data());
  return message;
}

std::expected<EchoResponse, std::string> DecodeEchoResponse(
    const mavlink_message_t& message, std::uint8_t expected_target_system,
    std::uint8_t expected_target_component) {
  if (message.msgid != MAVLINK_MSG_ID_TUNNEL ||
      expected_target_system == 0 || expected_target_component == 0) {
    return std::unexpected("不是有效回显TUNNEL帧");
  }
  mavlink_tunnel_t tunnel{};
  mavlink_msg_tunnel_decode(&message, &tunnel);
  if (tunnel.payload_type != kPayloadType ||
      tunnel.target_system != expected_target_system ||
      tunnel.target_component != expected_target_component ||
      tunnel.payload[0] != kVersion || tunnel.payload[1] != kResponseType) {
    return std::unexpected("回显帧类型、目标或版本不匹配");
  }
  const std::size_t text_size = tunnel.payload[10];
  const std::size_t logical_size = kTextOffset + text_size + kBaudBytes;
  if (text_size == 0 || text_size > kMaximumTextBytes ||
      tunnel.payload_length != logical_size ||
      message.len < kTunnelHeaderBytes + kTextOffset + text_size ||
      message.len > kTunnelHeaderBytes + logical_size) {
    return std::unexpected("回显帧长度非法");
  }
  // MAVLink 2 可裁剪末尾为零的波特率字节；逻辑长度仍以 payload_length 为准。
  EchoResponse response{
      .nonce = ReadNonce(tunnel.payload + 2),
      .text_bytes = {tunnel.payload + kTextOffset,
                     tunnel.payload + kTextOffset + text_size},
      .f407_baud_rate = ReadBaud(tunnel.payload + kTextOffset + text_size),
  };
  if (response.f407_baud_rate == 0) {
    return std::unexpected("F407报告的实际波特率非法");
  }
  return response;
}

std::string FrameHex(std::span<const std::uint8_t> wire_bytes) {
  constexpr char kDigits[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(wire_bytes.size() * 2);
  for (const auto byte : wire_bytes) {
    result.push_back(kDigits[byte >> 4]);
    result.push_back(kDigits[byte & 0x0F]);
  }
  return result;
}

}  // namespace experiment
