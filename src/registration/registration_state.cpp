/**
 * @file registration_state.cpp
 * @brief registration_state.hpp 的实现。
 */

#include "registration/registration_state.hpp"

namespace registration {

bool RegistrationState::ShouldPublish(bool connected, const std::string& current_payload) {
  if (connected && !was_connected_) {
    connection_requires_publish_ = true;
  }
  was_connected_ = connected;
  return connected &&
         (connection_requires_publish_ || !last_published_payload_ ||
          *last_published_payload_ != current_payload);
}

void RegistrationState::MarkPublished(const std::string& payload) {
  last_published_payload_ = payload;
  connection_requires_publish_ = false;
}

}  // namespace registration
