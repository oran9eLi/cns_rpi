#include "device_status/device_status.hpp"

#include <nlohmann/json.hpp>

namespace device_status {

std::string BuildPayload(const registration::OnlineRegistration& input,
                         const std::string& software_version, bool online,
                         const Metadata& metadata,
                         const std::string& health_status,
                         const std::optional<std::vector<std::string>>& fault_modules) {
  nlohmann::json payload{
      {"device_id", input.device_id},
      {"school_id", input.school_name.value_or("")},
      {"dcdw_label", nullptr},
      {"model_version", software_version},
      {"device_name", metadata.device_name},
      {"device_type", metadata.device_type},
      {"capabilities", metadata.capabilities},
      {"product", nullptr},
      {"online", online ? "0" : "1"},
      {"device_status", health_status},
      {"fault_modules", nullptr},
  };
  if (fault_modules) payload["fault_modules"] = *fault_modules;
  const auto& label = metadata.dcdw_label ? metadata.dcdw_label : input.dcdw_label;
  const auto& product = metadata.product ? metadata.product : input.product;
  if (label) payload["dcdw_label"] = *label;
  if (product) {
    payload["product"] = {
        {"manufacturer_code", product->manufacturer_code},
        {"model_code", product->model_code},
    };
  }
  return payload.dump();
}

bool PublicationState::ShouldPublish(bool mqtt_connected,
                                     std::uint64_t connection_generation,
                                     const std::string& payload,
                                     Clock::time_point now) const {
  return mqtt_connected &&
         (!last_payload_ || payload != *last_payload_ ||
          connection_generation != last_connection_generation_ ||
          now - last_published_at_ >= std::chrono::seconds(10));
}

void PublicationState::MarkPublished(std::uint64_t connection_generation,
                                     const std::string& payload,
                                     Clock::time_point now) {
  last_payload_ = payload;
  last_connection_generation_ = connection_generation;
  last_published_at_ = now;
}

}  // namespace device_status
