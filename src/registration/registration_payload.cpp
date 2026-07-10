/**
 * @file registration_payload.cpp
 * @brief registration_payload.hpp 的实现。
 */

#include "registration/registration_payload.hpp"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

namespace registration {

std::string BuildOnlinePayload(const OnlineRegistration& input) {
  nlohmann::json payload{
      {"schema_version", 1},
      {"vendor_id", input.vendor_id},
      {"school_name", input.school_name},
      {"status", "online"},
  };
  if (input.dcdw_label) {
    payload["dcdw_label"] = *input.dcdw_label;
  }
  return payload.dump();
}

std::string BuildOfflinePayload(const std::string& vendor_id) {
  return nlohmann::json{
      {"schema_version", 1},
      {"vendor_id", vendor_id},
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
