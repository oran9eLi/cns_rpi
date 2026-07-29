#pragma once

/**
 * @file registration_payload.hpp
 * @brief 构造 MQTT 设备注册消息，不负责 topic、连接或发布时机。
 */

#include <optional>
#include <string>

#include "common/mavlink.h"
#include "device/device_type.hpp"

namespace registration {

struct OnlineRegistration {
  std::string device_id;
  device::Type device_type = device::Type::kUnknown;
  std::optional<std::string> vendor_id;
  std::optional<std::string> school_name;
  std::optional<std::string> dcdw_label;
  std::optional<std::string> remote_id;
  std::optional<std::string> gateway_id;
  std::optional<std::uint8_t> system_id;
  std::optional<std::uint8_t> component_id;
  std::optional<std::uint8_t> mavlink_version;
  std::optional<mavlink_autopilot_version_t> autopilot_version;
};

std::string BuildOnlinePayload(const OnlineRegistration& input);
std::string BuildOfflinePayload(const std::string& device_id,
                                device::Type device_type);
std::string BuildClientId(const std::string& prefix, const std::string& vendor_id);
/// 检查前缀和设备标识能否安全用作 MQTT topic 单段与 Client ID。
bool IsValidDeviceIdentity(const std::string& prefix, const std::string& vendor_id);

}  // namespace registration
