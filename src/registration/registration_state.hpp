#pragma once

/**
 * @file registration_state.hpp
 * @brief 判断何时需要发布设备注册消息，不依赖 MQTT 或 JSON。
 */

#include <optional>
#include <string>

namespace registration {

class RegistrationState {
 public:
  bool ShouldPublish(bool connected, const std::string& current_payload);
  void MarkPublished(const std::string& payload);

 private:
  bool was_connected_ = false;
  bool connection_requires_publish_ = false;
  std::optional<std::string> last_published_payload_;
};

}  // namespace registration
