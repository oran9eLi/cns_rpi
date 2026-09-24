#pragma once

/**
 * @file m1_protocol.hpp
 * @brief M1 改参 TUNNEL 类型 3/4 的受限编解码，不负责串口读写。
 */

#include <cstdint>
#include <expected>
#include <string>

#include "common/mavlink.h"

namespace experiment {

struct BaudChangeAcceptance {
  std::uint64_t nonce{};
  std::uint32_t accepted_baud_rate{};
};

/** @brief 构造定向发送的类型 3 请求；只编码 57600/115200。 */
std::expected<mavlink_message_t, std::string> EncodeBaudChangeRequest(
    std::uint64_t nonce, std::uint32_t baud_rate,
    std::uint8_t source_system, std::uint8_t source_component,
    std::uint8_t target_system, std::uint8_t target_component);

/**
 * @brief 严格核对类型 4 的来源、目标、随机数及接受速率。
 * @return 仅表示 F407 在旧速率接受请求，不表示新速率已经应用。
 */
std::expected<BaudChangeAcceptance, std::string> DecodeBaudChangeAccepted(
    const mavlink_message_t& message,
    std::uint8_t expected_source_system, std::uint8_t expected_source_component,
    std::uint8_t expected_target_system, std::uint8_t expected_target_component,
    std::uint64_t expected_nonce, std::uint32_t expected_baud_rate);

}  // namespace experiment
