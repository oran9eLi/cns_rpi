#include "command_feedback/result_feedback.hpp"

#include <algorithm>
#include <array>

namespace command_feedback {

mavlink_message_t Encode(Result result, std::uint8_t source_system,
                         std::uint8_t source_component, std::uint8_t target_system,
                         std::uint8_t target_component) {
  std::array<std::uint8_t, MAVLINK_MSG_TUNNEL_FIELD_PAYLOAD_LEN> payload{};
  payload[0] = 1;
  payload[1] = static_cast<std::uint8_t>(result);
  mavlink_message_t message{};
  mavlink_msg_tunnel_pack(source_system, source_component, &message,
      target_system, target_component, 0x8040, 2, payload.data());
  return message;
}

std::optional<Result> ResultFeedback::Observe(std::string_view device_id, Channel channel,
                                             const nlohmann::json& ack) {
  const auto status = ack.value("status", std::string{});
  std::optional<Result> result;
  if ((channel == Channel::kConfig && (status == "applied" || status == "already_applied")) ||
      (channel == Channel::kControl && status == "accepted") ||
      (channel == Channel::kExperiment && status == "completed")) {
    result = Result::kSuccess;
  } else if (status == "rejected" || status == "timeout" || status == "failed" ||
             status == "cancelled") {
    result = Result::kFailure;
  }
  if (!result) return std::nullopt;
  if (device_id_ != device_id) {
    device_id_ = device_id;
    recent_.clear();
  }
  const auto id = ack.value(channel == Channel::kExperiment ? "action_id" : "command_id",
                            std::string{});
  if (id.empty()) return result;
  if (std::any_of(recent_.begin(), recent_.end(), [&](const auto& entry) {
        return entry.channel == channel && entry.id == id && entry.result == *result;
      })) return std::nullopt;
  if (recent_.size() == 128) recent_.pop_front();
  recent_.push_back({channel, id, *result});
  return result;
}

}  // namespace command_feedback
