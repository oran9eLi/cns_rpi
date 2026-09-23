#pragma once

/**
 * @file uart_echo_request.hpp
 * @brief H1 回显 MQTT 请求的严格解析边界，不接触串口或 MQTT 客户端。
 */

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace experiment {

struct UartEchoRequest {
  std::string command_id;
  std::string request_id;
  std::string device_id;
  std::string session_id;
  std::string action_id;
  std::uint64_t lease_version{};
  std::string expires_at;
  std::chrono::system_clock::time_point expiry;
  std::vector<std::uint8_t> text_bytes;
};

/** @brief 仅供现有实验 Topic 分流；严格字段校验仍由 ParseUartEchoRequest 执行。 */
bool IsUartEchoOperation(std::string_view payload);

/**
 * @brief 校验设备目标、动作身份、有效期和受限 UTF-8 文本。
 * @return 合法请求或中文拒绝原因；结构错误时调用方不得猜测 ACK 身份。
 */
std::expected<UartEchoRequest, std::string> ParseUartEchoRequest(
    std::string_view payload, std::string_view expected_device_id,
    std::chrono::system_clock::time_point now);

}  // namespace experiment
