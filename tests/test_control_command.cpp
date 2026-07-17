#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "control_command/control_command.hpp"

TEST_CASE("四路PWM命令解析并编码为31013") {
  auto command = control_command::Parse(
      R"({"command_id":"pwm-1","command":"set_motor_pwm","parameters":{"pwm_us":[1650,1651,1570,1530]}})");
  REQUIRE(command.has_value());
  CHECK(command->mavlink_command == control_command::kSetMotorPwmUs);
  CHECK(command->params[0] == 1650.0F);
  CHECK(command->params[3] == 1530.0F);

  const auto message = control_command::EncodeCommandLong(
      *command, 250, MAV_COMP_ID_ONBOARD_COMPUTER, 1, MAV_COMP_ID_AUTOPILOT1);
  mavlink_command_long_t packet{};
  mavlink_msg_command_long_decode(&message, &packet);
  CHECK(packet.command == 31013);
  CHECK(packet.target_system == 1);
  CHECK(packet.target_component == MAV_COMP_ID_AUTOPILOT1);
  CHECK(packet.param2 == 1651.0F);
}

TEST_CASE("起飞降落急停命令映射") {
  const auto takeoff = control_command::Parse(R"({"command_id":"1","command":"takeoff"})");
  const auto land = control_command::Parse(R"({"command_id":"2","command":"land","parameters":{}})");
  const auto stop = control_command::Parse(R"({"command_id":"3","command":"emergency_stop"})");
  REQUIRE(takeoff.has_value());
  REQUIRE(land.has_value());
  REQUIRE(stop.has_value());
  CHECK(takeoff->mavlink_command == 31091);
  CHECK(land->mavlink_command == 31092);
  CHECK(stop->mavlink_command == 31090);
}

TEST_CASE("拒绝越界PWM和未知命令") {
  CHECK_FALSE(control_command::Parse(
      R"({"command_id":"1","command":"set_motor_pwm","parameters":{"pwm_us":[999,1500,1500,1500]}})")
                  .has_value());
  CHECK_FALSE(control_command::Parse(R"({"command_id":"2","command":"hover"})").has_value());
}

TEST_CASE("按COMMAND_ACK生成服务器回执") {
  const auto command = control_command::Parse(R"({"command_id":"1","command":"takeoff"})");
  REQUIRE(command.has_value());
  auto accepted = control_command::BuildMavlinkAck(*command, MAV_RESULT_ACCEPTED);
  CHECK(accepted["status"] == "accepted");
  CHECK(accepted["mavlink_command"] == 31091);

  auto rejected = control_command::BuildMavlinkAck(*command, MAV_RESULT_TEMPORARILY_REJECTED);
  CHECK(rejected["status"] == "rejected");
  CHECK(rejected["result_code"] == "temporarily_rejected");

  auto progress = control_command::BuildMavlinkAck(*command, MAV_RESULT_IN_PROGRESS, 30, 7);
  CHECK(progress["status"] == "in_progress");
  CHECK(progress["progress"] == 30);
  CHECK(progress["result_param2"] == 7);
}

TEST_CASE("无参数命令拒绝多余参数") {
  const auto command = control_command::Parse(
      R"({"command_id":"1","command":"takeoff","parameters":{"height":10}})");
  REQUIRE_FALSE(command.has_value());
  CHECK(command.error().code == "invalid_parameter");
}

TEST_CASE("解析失败保留可用的命令关联信息") {
  const auto invalid_parameters = control_command::Parse(
      R"({"command_id":"pwm-invalid","command":"set_motor_pwm","parameters":{"pwm_us":[1500]}})");
  REQUIRE_FALSE(invalid_parameters.has_value());
  CHECK(invalid_parameters.error().command_id == "pwm-invalid");
  CHECK(invalid_parameters.error().command == "set_motor_pwm");

  const auto invalid_command = control_command::Parse(
      R"({"command_id":"bad-command","command":42})");
  REQUIRE_FALSE(invalid_command.has_value());
  CHECK(invalid_command.error().command_id == "bad-command");
  CHECK(invalid_command.error().command.empty());
}

TEST_CASE("PWM命令拒绝pwm_us以外的参数") {
  const auto command = control_command::Parse(
      R"({"command_id":"pwm-extra","command":"set_motor_pwm","parameters":{"pwm_us":[1500,1500,1500,1500],"motor_index":1}})");
  REQUIRE_FALSE(command.has_value());
  CHECK(command.error().code == "invalid_parameter");
  CHECK(command.error().command_id == "pwm-extra");
  CHECK(command.error().command == "set_motor_pwm");
}
