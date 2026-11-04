#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "control_command/control_endpoint.hpp"

namespace {

mavlink_message_t Heartbeat(std::uint8_t system_id, std::uint8_t component_id,
                            std::uint8_t type, std::uint8_t autopilot) {
  mavlink_message_t message{};
  mavlink_msg_heartbeat_pack(system_id, component_id, &message, type, autopilot,
                             0, 0, MAV_STATE_ACTIVE);
  return message;
}

mavlink_message_t CommandAck(std::uint8_t system_id, std::uint8_t component_id,
                             std::uint8_t target_system,
                             std::uint8_t target_component) {
  mavlink_message_t message{};
  mavlink_msg_command_ack_pack(system_id, component_id, &message, 31091,
                               MAV_RESULT_ACCEPTED, 0, 0, target_system,
                               target_component);
  return message;
}

}  // namespace

TEST_CASE("只从F407主控箱心跳学习STM32端点") {
  std::optional<control_command::MavlinkEndpoint> endpoint;
  endpoint = control_command::ObserveCnsBoxHeartbeat(
      Heartbeat(7, MAV_COMP_ID_ONBOARD_COMPUTER, MAV_TYPE_ONBOARD_CONTROLLER,
                MAV_AUTOPILOT_INVALID), endpoint);
  CHECK_FALSE(endpoint.has_value());

  endpoint = control_command::ObserveCnsBoxHeartbeat(
      Heartbeat(7, control_command::kStm32Usart6ComponentId,
                MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID), endpoint);
  REQUIRE(endpoint.has_value());
  CHECK(endpoint->system_id == 7);
  CHECK(endpoint->component_id == control_command::kStm32Usart6ComponentId);
}

TEST_CASE("STM32动态sysid只接受1到250") {
  const auto invalid_zero = control_command::ObserveCnsBoxHeartbeat(
      Heartbeat(0, control_command::kStm32Usart6ComponentId,
                MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID), std::nullopt);
  CHECK_FALSE(invalid_zero.has_value());

  const auto valid_max = control_command::ObserveCnsBoxHeartbeat(
      Heartbeat(250, control_command::kStm32Usart6ComponentId,
                MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID), std::nullopt);
  REQUIRE(valid_max.has_value());
  CHECK(valid_max->system_id == 250);

  const auto invalid_high = control_command::ObserveCnsBoxHeartbeat(
      Heartbeat(251, control_command::kStm32Usart6ComponentId,
                MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID), std::nullopt);
  CHECK_FALSE(invalid_high.has_value());
}

TEST_CASE("已学习端点不被其他心跳覆盖") {
  std::optional<control_command::MavlinkEndpoint> endpoint =
      control_command::MavlinkEndpoint{7, control_command::kStm32Usart6ComponentId};
  endpoint = control_command::ObserveCnsBoxHeartbeat(
      Heartbeat(8, control_command::kStm32Usart6ComponentId,
                MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID), endpoint);
  REQUIRE(endpoint.has_value());
  CHECK(endpoint->system_id == 7);
}

TEST_CASE("树莓派发帧使用已学习的STM32动态sysid") {
  CHECK_FALSE(control_command::LearnedSystemId(std::nullopt).has_value());

  const auto system_id = control_command::LearnedSystemId(control_command::MavlinkEndpoint{
      7, control_command::kStm32Usart6ComponentId});
  REQUIRE(system_id.has_value());
  CHECK(*system_id == 7);
}

TEST_CASE("COMMAND_ACK必须匹配来源和目标") {
  const control_command::MavlinkEndpoint endpoint{
      7, control_command::kStm32Usart6ComponentId};
  CHECK(control_command::IsExpectedCommandAck(
      CommandAck(7, control_command::kStm32Usart6ComponentId, 7,
                 MAV_COMP_ID_ONBOARD_COMPUTER),
      endpoint, 7, MAV_COMP_ID_ONBOARD_COMPUTER));
  CHECK_FALSE(control_command::IsExpectedCommandAck(
      CommandAck(8, control_command::kStm32Usart6ComponentId, 7,
                 MAV_COMP_ID_ONBOARD_COMPUTER),
      endpoint, 7, MAV_COMP_ID_ONBOARD_COMPUTER));
  CHECK_FALSE(control_command::IsExpectedCommandAck(
      CommandAck(7, control_command::kStm32Usart6ComponentId, 8,
                 MAV_COMP_ID_ONBOARD_COMPUTER),
      endpoint, 7, MAV_COMP_ID_ONBOARD_COMPUTER));
}

TEST_CASE("非主控箱心跳不会抢占端点且随后可学习F407主控箱") {
  std::optional<control_command::ControlledDeviceEndpoint> endpoint;
  for (const auto& message : {
           Heartbeat(2, MAV_COMP_ID_AUTOPILOT1, MAV_TYPE_QUADROTOR,
                     MAV_AUTOPILOT_PX4),
           Heartbeat(2, MAV_COMP_ID_AUTOPILOT1, MAV_TYPE_QUADROTOR,
                     MAV_AUTOPILOT_ARDUPILOTMEGA),
           Heartbeat(2, MAV_COMP_ID_ONBOARD_COMPUTER,
                     MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID),
           Heartbeat(2, control_command::kStm32Usart6ComponentId,
                     MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_INVALID),
           Heartbeat(2, control_command::kStm32Usart6ComponentId,
                     MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_PX4),
           Heartbeat(2, control_command::kStm32Usart6ComponentId,
                     MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_ARDUPILOTMEGA),
       }) {
    endpoint = control_command::ObserveControlledDeviceHeartbeat(
        message, endpoint);
    CHECK_FALSE(endpoint.has_value());
    CHECK_FALSE(control_command::ClassifyControlledDeviceHeartbeat(message)
                    .has_value());
  }

  endpoint = control_command::ObserveControlledDeviceHeartbeat(
      Heartbeat(7, control_command::kStm32Usart6ComponentId,
                MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID),
      endpoint);
  REQUIRE(endpoint.has_value());
  CHECK(endpoint->type == device::Type::kCnsBox);
  CHECK(endpoint->endpoint.system_id == 7);
  CHECK(endpoint->endpoint.component_id == control_command::kStm32Usart6ComponentId);
}

TEST_CASE("已确认F407主控箱只接受同system遥测并锁定控制组件") {
  const control_command::ControlledDeviceEndpoint controlled{
      .endpoint = {.system_id = 7,
                   .component_id = control_command::kStm32Usart6ComponentId},
      .type = device::Type::kCnsBox};

  mavlink_message_t telemetry{};
  mavlink_msg_global_position_int_pack(
      7, MAV_COMP_ID_GPS, &telemetry, 1, 0, 0, 0, 0, 0, 0, 0, 0);
  CHECK(control_command::IsMessageFromControlledDevice(telemetry, controlled));

  CHECK_FALSE(control_command::IsMessageFromControlledDevice(
      Heartbeat(7, MAV_COMP_ID_GPS, MAV_TYPE_GPS, MAV_AUTOPILOT_INVALID),
      controlled));
  CHECK_FALSE(control_command::IsMessageFromControlledDevice(
      Heartbeat(8, control_command::kStm32Usart6ComponentId,
                MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID),
      controlled));
  CHECK_FALSE(control_command::IsMessageFromControlledDevice(
      CommandAck(7, MAV_COMP_ID_GPS, 7, MAV_COMP_ID_ONBOARD_COMPUTER), controlled));
  CHECK(control_command::IsMessageFromControlledDevice(
      CommandAck(7, control_command::kStm32Usart6ComponentId,
                 7, MAV_COMP_ID_ONBOARD_COMPUTER), controlled));
}
