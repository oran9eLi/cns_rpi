#pragma once

/**
 * @file m1_request.hpp
 * @brief M1 实验 MQTT 请求的严格解析边界，不接触串口或 MQTT 客户端。
 */

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace experiment {

enum class M1Operation { kSetF407Baud, kSetPiBaud, kUartProbe };

struct M1Request {
  std::string command_id;
  std::string request_id;
  std::string device_id;
  std::string session_id;
  std::string action_id;
  std::uint64_t lease_version{};
  M1Operation operation{};
  std::optional<int> baud_rate;
  std::string expires_at;
  std::chrono::system_clock::time_point expiry;
};

/** @brief 仅供实验 Topic 分流；完整字段校验由 ParseM1Request 完成。 */
bool IsM1Operation(std::string_view payload);

/**
 * @brief 严格校验三种 M1 操作、本机目标、动作身份和有效期格式。
 * @return 合法请求或中文错误原因；动作是否过期留给持久化事务层处理。
 */
std::expected<M1Request, std::string> ParseM1Request(
    std::string_view payload, std::string_view expected_device_id);

}  // namespace experiment
