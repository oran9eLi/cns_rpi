#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "config_command/command_parser.hpp"

using config_command::ParseConfigCommand;

TEST_CASE("合法部分更新解析为类型安全补丁") {
  auto result = ParseConfigCommand(R"({
    "command_id":"cmd-001",
    "parameters":{"telemetry_publish_interval_ms":2000}
  })");
  REQUIRE(result.has_value());
  CHECK(result->command_id == "cmd-001");
  CHECK(result->parameters.telemetry_publish_interval_ms == 2000);
  CHECK_FALSE(result->parameters.heartbeat_interval_ms.has_value());
}

TEST_CASE("非法JSON和命令号被拒绝") {
  CHECK(ParseConfigCommand("{").error().code == "invalid_json");
  CHECK(ParseConfigCommand(R"({"parameters":{"heartbeat_interval_ms":1000}})")
            .error().code == "invalid_command_id");
  CHECK(ParseConfigCommand(R"({"command_id":"","parameters":{"heartbeat_interval_ms":1000}})")
            .error().code == "invalid_command_id");
  CHECK(ParseConfigCommand(R"({"command_id":1,"parameters":{"heartbeat_interval_ms":1000}})")
            .error().code == "invalid_command_id");
  const std::string long_id(129, 'x');
  CHECK(ParseConfigCommand("{\"command_id\":\"" + long_id +
                           "\",\"parameters\":{\"heartbeat_interval_ms\":1000}}")
            .error().code == "invalid_command_id");
}

TEST_CASE("未知参数空参数和类型错误被拒绝") {
  CHECK(ParseConfigCommand(R"({"command_id":"cmd-1","source_id":"forged",
      "parameters":{"heartbeat_interval_ms":1000}})")
            .error().code == "unknown_parameter");
  CHECK(ParseConfigCommand(
      R"({"command_id":"cmd-1","parameters":{"serial_baud":9600}})")
            .error().code == "unknown_parameter");
  CHECK(ParseConfigCommand(R"({"command_id":"cmd-1","parameters":{}})")
            .error().code == "invalid_parameter");
  CHECK(ParseConfigCommand(
      R"({"command_id":"cmd-1","parameters":{"heartbeat_interval_ms":"1000"}})")
            .error().code == "invalid_parameter_type");
  CHECK(ParseConfigCommand(
      R"({"command_id":"cmd-1","parameters":{"heartbeat_interval_ms":1000.0}})")
            .error().code == "invalid_parameter_type");
}

TEST_CASE("四个参数边界和重连组合得到校验") {
  CHECK(ParseConfigCommand(
      R"({"command_id":"cmd-1","parameters":{"heartbeat_interval_ms":99}})")
            .error().code == "invalid_parameter");
  CHECK(ParseConfigCommand(
      R"({"command_id":"cmd-1","parameters":{"telemetry_publish_interval_ms":60001}})")
            .error().code == "invalid_parameter");
  CHECK(ParseConfigCommand(
      R"({"command_id":"cmd-1","parameters":{"mqtt_reconnect_delay_s":0}})")
            .error().code == "invalid_parameter");
  CHECK(ParseConfigCommand(
      R"({"command_id":"cmd-1","parameters":{"mqtt_reconnect_delay_max_s":3601}})")
            .error().code == "invalid_parameter");
  CHECK(ParseConfigCommand(R"({"command_id":"cmd-1","parameters":{
      "mqtt_reconnect_delay_s":31,"mqtt_reconnect_delay_max_s":30}})")
            .error().code == "invalid_parameter");

  auto boundaries = ParseConfigCommand(R"({"command_id":"cmd-2","parameters":{
      "telemetry_publish_interval_ms":100,"heartbeat_interval_ms":60000,
      "mqtt_reconnect_delay_s":1,"mqtt_reconnect_delay_max_s":3600}})");
  REQUIRE(boundaries.has_value());
}

TEST_CASE("拒绝ACK包含稳定错误结构") {
  auto ack = config_command::BuildRejectedAck(
      "cmd-1", {.code = "invalid_parameter", .message = "参数非法"});
  CHECK(ack == nlohmann::json{{"command_id", "cmd-1"},
                              {"status", "rejected"},
                              {"error_code", "invalid_parameter"},
                              {"message", "参数非法"},
                              {"restart_required", false}});
}
