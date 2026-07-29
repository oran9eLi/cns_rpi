/**
 * @file registration_payload.cpp
 * @brief registration_payload.hpp 的实现。
 */

#include "registration/registration_payload.hpp"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "protocol/px4_identity.hpp"

namespace registration {

std::string BuildOnlinePayload(const OnlineRegistration& input) {
  nlohmann::json payload{
      {"schema_version", 2},
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

  nlohmann::json identity = nlohmann::json::object();
  if (input.vendor_id) {
    identity["vendor_id"] = *input.vendor_id;
  }
  if (input.school_name && !input.school_name->empty()) {
    identity["school_name"] = *input.school_name;
  }
  if (input.dcdw_label) {
    identity["dcdw_label"] = *input.dcdw_label;
  }
  if (input.remote_id) {
    identity["remote_id"] = *input.remote_id;
  }
  if (input.autopilot_version) {
    const auto& version = *input.autopilot_version;
    if (const auto uid2 = protocol::FormatPx4Uid2(version)) {
      identity["uid2"] = *uid2;
    }
    if (const auto uid = protocol::FormatPx4Uid(version)) {
      identity["uid"] = *uid;
    }
    identity["autopilot"] = "PX4";
    identity["firmware_version"] =
        std::to_string((version.flight_sw_version >> 24) & 0xFFU) + "." +
        std::to_string((version.flight_sw_version >> 16) & 0xFFU) + "." +
        std::to_string((version.flight_sw_version >> 8) & 0xFFU);
    identity["hardware_vendor_id"] = version.vendor_id;
    identity["hardware_product_id"] = version.product_id;
    identity["board_version"] = version.board_version;
  }
  payload["identity"] = std::move(identity);

  nlohmann::json endpoint = nlohmann::json::object();
  if (input.system_id) {
    endpoint["sysid"] = *input.system_id;
  }
  if (input.component_id) {
    endpoint["compid"] = *input.component_id;
  }
  if (input.mavlink_version) {
    endpoint["mavlink_version"] = *input.mavlink_version;
  }
  payload["endpoint"] = std::move(endpoint);

  nlohmann::json gateway{
      {"software_version", "cns_rpi-2.0.0"},
      {"connection", "5g"},
  };
  if (input.gateway_id) {
    gateway["gateway_id"] = *input.gateway_id;
  }
  payload["gateway"] = std::move(gateway);
  return payload.dump();
}

std::string BuildOfflinePayload(const std::string& device_id,
                                device::Type device_type) {
  return nlohmann::json{
      {"schema_version", 2},
      {"device_id", device_id},
      {"device_type", std::string(device::TypeName(device_type))},
      {"status", "offline"},
  }.dump();
}

std::string BuildClientId(const std::string& prefix, const std::string& vendor_id) {
  return prefix + "-" + vendor_id;
}

bool IsValidDeviceIdentity(const std::string& prefix, const std::string& vendor_id) {
  const auto safe = [](const std::string& value) {
    return !value.empty() && std::ranges::all_of(value, [](unsigned char ch) {
      return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == ':';
    });
  };
  return safe(prefix) && safe(vendor_id) && BuildClientId(prefix, vendor_id).size() <= 65535;
}

}  // namespace registration
