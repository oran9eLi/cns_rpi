#pragma once

/**
 * @file registration_payload.hpp
 * @brief 构造 MQTT 设备注册消息，不负责 topic、连接或发布时机。
 */

#include <optional>
#include <string>

#include "device/device_type.hpp"
#include "device/product_info.hpp"

namespace registration {

/// 注册载荷只携带受控设备身份和元数据。树莓派不是平台管理对象，
/// 它的序列号、MAVLink 端点和软件版本都不进这个结构(设计文档 §5.3)。
struct OnlineRegistration {
  std::string device_id;
  device::Type device_type = device::Type::kUnknown;
  std::optional<std::string> school_name;
  std::optional<std::string> dcdw_label;
  std::optional<device::ProductInfo> product;
  std::optional<device::VersionInfo> version;
};

std::string BuildOnlinePayload(const OnlineRegistration& input);
std::string BuildOfflinePayload(const std::string& device_id,
                                device::Type device_type);
std::string BuildClientId(const std::string& prefix, const std::string& device_id);
/// 检查前缀和设备标识能否安全用作 MQTT topic 单段与 Client ID。
bool IsValidDeviceIdentity(const std::string& prefix, const std::string& device_id);

}  // namespace registration
