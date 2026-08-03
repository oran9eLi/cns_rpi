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

TEST_CASE("只从飞控心跳学习STM32端点") {
  std::optional<control_command::MavlinkEndpoint> endpoint;
  endpoint = control_command::ObserveFlightControllerHeartbeat(
      Heartbeat(7, MAV_COMP_ID_ONBOARD_COMPUTER, MAV_TYPE_ONBOARD_CONTROLLER,
                MAV_AUTOPILOT_INVALID), endpoint);
  CHECK_FALSE(endpoint.has_value());

  endpoint = control_command::ObserveFlightControllerHeartbeat(
      Heartbeat(7, control_command::kStm32Usart6ComponentId,
                MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID), endpoint);
  REQUIRE(endpoint.has_value());
  CHECK(endpoint->system_id == 7);
  CHECK(endpoint->component_id == control_command::kStm32Usart6ComponentId);
}

TEST_CASE("STM32动态sysid只接受1到250") {
  const auto invalid_zero = control_command::ObserveFlightControllerHeartbeat(
      Heartbeat(0, control_command::kStm32Usart6ComponentId,
                MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID), std::nullopt);
  CHECK_FALSE(invalid_zero.has_value());

  const auto valid_max = control_command::ObserveFlightControllerHeartbeat(
      Heartbeat(250, control_command::kStm32Usart6ComponentId,
                MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID), std::nullopt);
  REQUIRE(valid_max.has_value());
  CHECK(valid_max->system_id == 250);

  const auto invalid_high = control_command::ObserveFlightControllerHeartbeat(
      Heartbeat(251, control_command::kStm32Usart6ComponentId,
                MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID), std::nullopt);
  CHECK_FALSE(invalid_high.has_value());
}

TEST_CASE("已学习端点不被其他心跳覆盖") {
  std::optional<control_command::MavlinkEndpoint> endpoint =
      control_command::MavlinkEndpoint{7, control_command::kStm32Usart6ComponentId};
  endpoint = control_command::ObserveFlightControllerHeartbeat(
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

TEST_CASE("auto mode identifies cns box and px4 with strict heartbeat signatures") {
  const auto cns_box = control_command::ObserveControlledDeviceHeartbeat(
      Heartbeat(7, control_command::kStm32Usart6ComponentId,
                MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID),
      std::nullopt, device::DetectionMode::kAuto);
  REQUIRE(cns_box.has_value());
  CHECK(cns_box->type == device::Type::kCnsBox);

  const auto px4 = control_command::ObserveControlledDeviceHeartbeat(
      Heartbeat(2, MAV_COMP_ID_AUTOPILOT1, MAV_TYPE_QUADROTOR,
                MAV_AUTOPILOT_PX4),
      std::nullopt, device::DetectionMode::kAuto);
  REQUIRE(px4.has_value());
  CHECK(px4->type == device::Type::kFlightController);
  CHECK(px4->endpoint.system_id == 2);
  const auto independently_classified =
      control_command::ClassifyControlledDeviceHeartbeat(
          Heartbeat(2, MAV_COMP_ID_AUTOPILOT1, MAV_TYPE_QUADROTOR,
                    MAV_AUTOPILOT_PX4),
          device::DetectionMode::kAuto);
  REQUIRE(independently_classified.has_value());
  CHECK(independently_classified->type ==
        device::Type::kFlightController);

  CHECK_FALSE(control_command::ObserveControlledDeviceHeartbeat(
                  Heartbeat(9, MAV_COMP_ID_AUTOPILOT1, MAV_TYPE_QUADROTOR,
                            MAV_AUTOPILOT_ARDUPILOTMEGA),
                  std::nullopt, device::DetectionMode::kAuto)
                  .has_value());
}

TEST_CASE("explicit mode only accepts its configured device family") {
  CHECK_FALSE(control_command::ObserveControlledDeviceHeartbeat(
                  Heartbeat(2, MAV_COMP_ID_AUTOPILOT1, MAV_TYPE_QUADROTOR,
                            MAV_AUTOPILOT_PX4),
                  std::nullopt, device::DetectionMode::kCnsBox)
                  .has_value());

  const auto forced_px4 = control_command::ObserveControlledDeviceHeartbeat(
      Heartbeat(2, MAV_COMP_ID_AUTOPILOT1, MAV_TYPE_QUADROTOR,
                MAV_AUTOPILOT_INVALID),
      std::nullopt, device::DetectionMode::kPx4);
  REQUIRE(forced_px4.has_value());
  CHECK(forced_px4->type == device::Type::kFlightController);
}

TEST_CASE("controlled system accepts telemetry components but pins control messages") {
  const control_command::ControlledDeviceEndpoint controlled{
      .endpoint = {.system_id = 2, .component_id = MAV_COMP_ID_AUTOPILOT1},
      .type = device::Type::kFlightController};

  mavlink_message_t telemetry{};
  mavlink_msg_global_position_int_pack(
      2, MAV_COMP_ID_GPS, &telemetry, 1, 0, 0, 0, 0, 0, 0, 0, 0);
  CHECK(control_command::IsMessageFromControlledDevice(telemetry, controlled));

  CHECK_FALSE(control_command::IsMessageFromControlledDevice(
      Heartbeat(2, MAV_COMP_ID_GPS, MAV_TYPE_GPS, MAV_AUTOPILOT_INVALID),
      controlled));
  CHECK_FALSE(control_command::IsMessageFromControlledDevice(
      Heartbeat(3, MAV_COMP_ID_AUTOPILOT1, MAV_TYPE_QUADROTOR,
                MAV_AUTOPILOT_PX4),
      controlled));
}
