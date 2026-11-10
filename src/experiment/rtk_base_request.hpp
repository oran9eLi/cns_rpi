/**
 * @file rtk_base_request.hpp
 * @brief RTK 基站实验的 JSON 入口；执行层只接收已经校验的类型化请求。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace experiment {

/// 与现有 experiment/set 动作共用关联字段；状态查询不携带测量参数。
struct RtkBaseRequest {
  std::string command_id, device_id, session_id, action_id, operation;
  std::uint64_t lease_version{0};
  std::chrono::system_clock::time_point expiry;
  std::string comparison;                  ///< 去掉 command_id 后的规范 JSON，供同动作去重。
  std::uint32_t minimum_duration_s{0};     ///< Survey-in 最短时间，10..86400 秒。
  double accuracy_limit_m{0.0};           ///< Survey-in 平均位置精度门限，0.1..100 米。
};

/// MQTT 分流前识别操作名；完整校验必须继续调用 ParseRtkBaseRequest。
bool IsRtkBaseOperation(std::string_view payload);

/**
 * @brief 校验实验外层和显式 Survey-in 参数；不访问设备、不校验当前租约或当前时间。
 * @return 成功返回类型化请求；失败返回可用于动作 ACK 的中文原因。
 */
std::expected<RtkBaseRequest, std::string> ParseRtkBaseRequest(std::string_view payload);

}  // namespace experiment
