#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <string>

#include "command_feedback/result_feedback.hpp"
#include "uart/mavlink_link.hpp"

using command_feedback::Channel;
using command_feedback::Result;
using command_feedback::ResultFeedback;

TEST_CASE("Command feedback maps only terminal results for each channel") {
  ResultFeedback feedback;
  CHECK(feedback.Observe("box", Channel::kConfig,
      {{"command_id", "c1"}, {"status", "applied"}}) == Result::kSuccess);
  CHECK(feedback.Observe("box", Channel::kConfig,
      {{"command_id", "c2"}, {"status", "already_applied"}}) == Result::kSuccess);
  CHECK(feedback.Observe("box", Channel::kControl,
      {{"command_id", "c3"}, {"status", "accepted"}, {"result", 0}}) == Result::kSuccess);
  CHECK(feedback.Observe("box", Channel::kExperiment,
      {{"action_id", "a1"}, {"status", "completed"}}) == Result::kSuccess);
  for (const auto channel : {Channel::kConfig, Channel::kControl, Channel::kExperiment}) {
    for (const auto* status : {"rejected", "timeout", "failed", "cancelled"}) {
      CAPTURE(status);
      CHECK(feedback.Observe("box", channel, {{"status", status}}) == Result::kFailure);
    }
    for (const auto* status : {"in_progress", "pending", "unknown"}) {
      CHECK_FALSE(feedback.Observe("box", channel, {{"status", status}}));
    }
  }
  CHECK_FALSE(feedback.Observe("box", Channel::kConfig, {{"status", "accepted"}}));
  CHECK_FALSE(feedback.Observe("box", Channel::kControl, {{"status", "completed"}}));
}

TEST_CASE("Repeated ACK delivery does not repeat feedback") {
  ResultFeedback feedback;
  const nlohmann::json config{{"command_id", "rate"}, {"status", "applied"}};
  CHECK(feedback.Observe("box", Channel::kConfig, config) == Result::kSuccess);
  CHECK_FALSE(feedback.Observe("box", Channel::kConfig, config));
  CHECK_FALSE(feedback.Observe("box", Channel::kConfig,
      {{"command_id", "rate"}, {"status", "already_applied"}}));
  CHECK(feedback.Observe("box", Channel::kControl,
      {{"command_id", "rate"}, {"status", "accepted"}}) == Result::kSuccess);
  CHECK(feedback.Observe("box", Channel::kConfig,
      {{"command_id", "rate"}, {"status", "rejected"}}) == Result::kFailure);
  CHECK(feedback.Observe("box", Channel::kExperiment,
      {{"command_id", "delivery1"}, {"action_id", "action"}, {"status", "completed"}})
      == Result::kSuccess);
  CHECK_FALSE(feedback.Observe("box", Channel::kExperiment,
      {{"command_id", "delivery2"}, {"action_id", "action"}, {"status", "completed"}}));
  CHECK(feedback.Observe("another-box", Channel::kConfig, config) == Result::kSuccess);
}

TEST_CASE("Feedback history is bounded and invalid requests without IDs still fail") {
  ResultFeedback feedback;
  for (int index = 0; index < 129; ++index) {
    CHECK(feedback.Observe("box", Channel::kControl,
        {{"command_id", std::to_string(index)}, {"status", "accepted"}})
        == Result::kSuccess);
  }
  CHECK_FALSE(feedback.Observe("box", Channel::kControl,
      {{"command_id", "128"}, {"status", "accepted"}}));
  CHECK(feedback.Observe("box", Channel::kControl,
      {{"command_id", "0"}, {"status", "accepted"}}) == Result::kSuccess);
  for (int attempt = 0; attempt < 2; ++attempt) {
    CHECK(feedback.Observe("box", Channel::kExperiment, {{"status", "rejected"}})
        == Result::kFailure);
  }
}

TEST_CASE("Both feedback values survive official MAVLink encoding and parsing") {
  for (const auto result : {Result::kFailure, Result::kSuccess}) {
    const auto message = command_feedback::Encode(result, 218, 191, 218, 193);
    CHECK(message.msgid == MAVLINK_MSG_ID_TUNNEL);
    CHECK(message.sysid == 218);
    CHECK(message.compid == 191);
    std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> bytes{};
    const auto size = mavlink_msg_to_send_buffer(bytes.data(), &message);
    uart::MavlinkFrameAssembler assembler;
    const auto parsed = assembler.Feed({bytes.data(), size});
    REQUIRE(parsed);
    mavlink_tunnel_t tunnel{};
    mavlink_msg_tunnel_decode(&*parsed, &tunnel);
    CHECK(tunnel.payload_type == 0x8040);
    CHECK(tunnel.payload_length == 2);
    CHECK(tunnel.target_system == 218);
    CHECK(tunnel.target_component == 193);
    CHECK(tunnel.payload[0] == 1);
    CHECK(tunnel.payload[1] == static_cast<std::uint8_t>(result));
    if (result == Result::kFailure) CHECK(parsed->len == 6);
  }
}
