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

/// 注册与常规遥测的协议版本。统一身份删除了字段并改变了 PX4 device_id 语义，
/// 属于不兼容变更，从 2 升到 3(设计文档 §5.2)。
constexpr int kSchemaVersion = 3;

}  // namespace

std::string BuildOnlinePayload(const OnlineRegistration& input) {
  nlohmann::json payload{
      {"schema_version", kSchemaVersion},
      {"device_id", input.device_id},
      {"device_type", std::string(device::TypeName(input.device_type))},
      {"status", "online"},
  };

  nlohmann::json capabilities = {"telemetry", "remote_id"};
  if (input.device_type == device::Type::kCnsBox) {
    capabilities.push_back("runtime_config");
    capabilities.push_back("box_private_control");
  } else if (input.device_type == device::Type::kFlightController) {
    capabilities.push_back("px4_official_control");
  }
  payload["capabilities"] = std::move(capabilities);

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
  if (input.version) {
    nlohmann::json version = nlohmann::json::object();
    if (input.version->hardware) {
      version["hardware"] = *input.version->hardware;
    }
    if (input.version->firmware) {
      version["firmware"] = *input.version->firmware;
    }
    if (!version.empty()) {
      payload["version"] = std::move(version);
    }
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
