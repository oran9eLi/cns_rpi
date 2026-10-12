#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

#include "control_command/endpoint_revalidation.hpp"

using namespace std::chrono_literals;

namespace {
constexpr auto kStarted = std::chrono::steady_clock::time_point{};
constexpr auto kDeviceId = "DCDWCNS1GHC0G6LF8MY6";

control_command::ControlledDeviceEndpoint Box(std::uint8_t sysid,
                                               std::uint8_t compid = 193) {
  return {{sysid, compid}, device::Type::kCnsBox};
}

mavlink_message_t BasicId(std::uint8_t sysid, std::uint8_t compid,
                          const std::string& device_id) {
  std::uint8_t uas_id[20]{};
  std::uint8_t id_or_mac[20]{};
  std::copy(device_id.begin(), device_id.end(), uas_id);
  mavlink_message_t message{};
  mavlink_msg_open_drone_id_basic_id_pack(sysid, compid, &message, sysid,
                                          compid, id_or_mac, 0, 0, uas_id);
  return message;
}
}  // namespace

TEST_CASE("同一F407换system_id后只登记候选而不直接放行") {
  control_command::EndpointRevalidation transition;
  CHECK(transition.ObserveHeartbeat(Box(76), Box(77), true, kStarted) ==
        control_command::HeartbeatObservation::kCandidate);
  CHECK(transition.Pending());
  REQUIRE(transition.Candidate().has_value());
  CHECK(transition.Candidate()->system_id == 77);
  CHECK(transition.ObserveBasicId(BasicId(76, 193, kDeviceId), kDeviceId) ==
        control_command::BasicIdObservation::kIgnored);
  CHECK(transition.Pending());
  CHECK(transition.ObserveBasicId(BasicId(77, 194, kDeviceId), kDeviceId) ==
        control_command::BasicIdObservation::kIgnored);
  CHECK(transition.ObserveBasicId(BasicId(77, 193, kDeviceId), kDeviceId) ==
        control_command::BasicIdObservation::kVerified);
}

TEST_CASE("未绑定和非主控箱不进入平滑换号") {
  control_command::EndpointRevalidation transition;
  CHECK(transition.ObserveHeartbeat(Box(76), Box(77), false, kStarted) ==
        control_command::HeartbeatObservation::kUnchanged);
  CHECK_FALSE(transition.Pending());
  auto px4 = Box(77);
  px4.type = device::Type::kFlightController;
  CHECK(transition.ObserveHeartbeat(Box(76), px4, true, kStarted) ==
        control_command::HeartbeatObservation::kUnchanged);
  CHECK(transition.ObserveHeartbeat(Box(76), Box(77, 194), true, kStarted) ==
        control_command::HeartbeatObservation::kUnchanged);
}

TEST_CASE("候选身份冲突不能改绑或放行") {
  control_command::EndpointRevalidation transition;
  REQUIRE(transition.ObserveHeartbeat(Box(76), Box(77), true, kStarted) ==
          control_command::HeartbeatObservation::kCandidate);
  CHECK(transition.ObserveBasicId(BasicId(77, 193, "DCDWCNS1OTHERDEVICE"),
                                  kDeviceId) ==
        control_command::BasicIdObservation::kConflict);
  CHECK(transition.Pending());
}

TEST_CASE("第三端点使候选歧义且迟到旧帧不能取消暂停") {
  control_command::EndpointRevalidation transition;
  REQUIRE(transition.ObserveHeartbeat(Box(76), Box(77), true, kStarted) ==
          control_command::HeartbeatObservation::kCandidate);
  CHECK(transition.ObserveHeartbeat(Box(76), Box(76), true, kStarted + 1s) ==
        control_command::HeartbeatObservation::kUnchanged);
  CHECK(transition.Pending());
  CHECK(transition.ObserveHeartbeat(Box(76), Box(78), true, kStarted + 1s) ==
        control_command::HeartbeatObservation::kAmbiguous);
  CHECK(transition.ObserveBasicId(BasicId(77, 193, kDeviceId), kDeviceId) ==
        control_command::BasicIdObservation::kIgnored);
}

TEST_CASE("三秒仅发一次告警且不会自动解除待复核") {
  control_command::EndpointRevalidation transition;
  REQUIRE(transition.ObserveHeartbeat(Box(76), Box(77), true, kStarted) ==
          control_command::HeartbeatObservation::kCandidate);
  CHECK_FALSE(transition.ShouldWarn(kStarted + 2999ms));
  CHECK(transition.ShouldWarn(kStarted + 3s));
  CHECK_FALSE(transition.ShouldWarn(kStarted + 4s));
  CHECK(transition.Pending());
}
