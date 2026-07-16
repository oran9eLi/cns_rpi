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

  const auto message = control_command::EncodeCommandLong(*command, 250, MAV_COMP_ID_ONBOARD_COMPUTER);
  mavlink_command_long_t packet{};
  mavlink_msg_command_long_decode(&message, &packet);
  CHECK(packet.command == 31013);
  CHECK(packet.target_system == 0);
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
}
