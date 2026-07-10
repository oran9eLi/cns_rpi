#pragma once

/**
 * @file registration_payload.hpp
 * @brief 构造 MQTT 设备注册消息，不负责 topic、连接或发布时机。
 */

#include <optional>
#include <string>

namespace registration {

struct OnlineRegistration {
  std::string vendor_id;
  std::string school_name;
  std::optional<std::string> dcdw_label;
};

std::string BuildOnlinePayload(const OnlineRegistration& input);
std::string BuildOfflinePayload(const std::string& vendor_id);
std::string BuildClientId(const std::string& prefix, const std::string& vendor_id);

}  // namespace registration
