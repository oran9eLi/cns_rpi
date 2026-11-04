/**
 * @file registration_payload.cpp
 * @brief registration_payload.hpp 的实现。
 */

#include "registration/registration_payload.hpp"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

namespace registration {

namespace {

/// 注册与完整遥测快照使用统一设备身份的第三版结构。
constexpr int kSchemaVersion = 3;

}  // namespace

std::string BuildOnlinePayload(const OnlineRegistration& input) {
  nlohmann::json payload{
      {"schema_version", kSchemaVersion},
      {"device_id", input.device_id},
      {"device_type", std::string(device::TypeName(input.device_type))},
      {"status", "online"},
  };

  payload["capabilities"] = {"telemetry", "remote_id", "runtime_config", "box_private_control"};

  if (input.school_name && !input.school_name->empty()) {
    payload["school_name"] = *input.school_name;
  }
  if (input.dcdw_label) {
    payload["dcdw_label"] = *input.dcdw_label;
  }
  if (input.product) {
    payload["product"] = {
        {"manufacturer_code", input.product->manufacturer_code},
        {"model_code", input.product->model_code},
    };
  }
  return payload.dump();
}

std::string BuildOfflinePayload(const std::string& device_id,
                                device::Type device_type) {
  // 只携带定位 retained 状态所必需的字段：不能用空值覆盖服务器已有元数据。
  return nlohmann::json{
      {"schema_version", kSchemaVersion},
      {"device_id", device_id},
      {"device_type", std::string(device::TypeName(device_type))},
      {"status", "offline"},
  }.dump();
}

std::string BuildClientId(const std::string& prefix, const std::string& device_id) {
  return prefix + "-" + device_id;
}

bool IsValidDeviceIdentity(const std::string& prefix, const std::string& device_id) {
  const auto safe = [](const std::string& value) {
    return !value.empty() && std::ranges::all_of(value, [](unsigned char ch) {
      return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == ':';
    });
  };
  return safe(prefix) && safe(device_id) && BuildClientId(prefix, device_id).size() <= 65535;
}

}  // namespace registration
