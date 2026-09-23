#pragma once

/**
 * @file uart_echo_protocol.hpp
 * @brief 候选 H1 TUNNEL 载荷编解码；不负责请求准入或串口 I/O。
 */

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "common/mavlink.h"

namespace experiment {

struct EchoResponse {
  std::uint64_t nonce{};
  std::vector<std::uint8_t> text_bytes;
  std::uint32_t f407_baud_rate{};
};

/** @brief 按候选 TUNNEL 格式构造发给已学习 F407 端点的请求。 */
std::expected<mavlink_message_t, std::string> EncodeEchoRequest(
    std::span<const std::uint8_t> text_bytes, std::uint64_t nonce,
    std::uint8_t source_system, std::uint8_t source_component,
    std::uint8_t target_system, std::uint8_t target_component);

/** @brief 严格解析候选 F407 响应，核对 TUNNEL 的 Pi 目标端点。 */
std::expected<EchoResponse, std::string> DecodeEchoResponse(
    const mavlink_message_t& message, std::uint8_t expected_target_system,
    std::uint8_t expected_target_component);

/** @brief 将已经取得的完整线上帧字节转换为大写十六进制。 */
std::string FrameHex(std::span<const std::uint8_t> wire_bytes);

}  // namespace experiment
