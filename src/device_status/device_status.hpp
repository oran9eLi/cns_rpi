#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "registration/registration_payload.hpp"
#include "device_status/metadata.hpp"

namespace device_status {

using Clock = std::chrono::steady_clock;

/// 复用注册入口已经按绑定身份筛选的元数据，不重新校验设备身份。
std::string BuildPayload(const registration::OnlineRegistration& device,
                         const std::string& software_version, bool online,
                         const Metadata& metadata = {},
                         const std::string& health_status = "0",
                         const std::optional<std::vector<std::string>>& fault_modules = std::nullopt);

class PublicationState {
 public:
  void SetInterval(std::chrono::milliseconds interval) { interval_ = interval; }
  bool ShouldPublish(bool mqtt_connected, std::uint64_t connection_generation,
                     const std::string& payload, Clock::time_point now) const;
  void MarkPublished(std::uint64_t connection_generation,
                     const std::string& payload, Clock::time_point now);

 private:
  std::optional<std::string> last_payload_;
  std::uint64_t last_connection_generation_{0};
  Clock::time_point last_published_at_{};
  std::chrono::milliseconds interval_{10000};
};

}  // namespace device_status
