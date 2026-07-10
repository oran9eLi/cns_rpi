/**
 * @file registration_payload.cpp
 * @brief registration_payload.hpp 的实现。
 */

#include "registration/registration_payload.hpp"

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

}  // namespace registration
