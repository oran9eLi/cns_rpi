#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <string>
#include <utility>

#include "control_command/control_command.hpp"
#include "control_command/control_endpoint.hpp"

TEST_CASE("单电机PWM命令解析并编码为31014") {
  const auto command = control_command::Parse(
      R"({"command_id":"pwm-1","command":"set_motor_pwm","parameters":{"motor_id":2,"pwm_us":1300}})");
  REQUIRE(command.has_value());
  CHECK(command->mavlink_command == 31014);
  CHECK(command->params == std::array<float, 4>{2.0F, 1300.0F, 0.0F, 0.0F});

  const auto message = control_command::EncodeCommandLong(
      *command, 7, MAV_COMP_ID_ONBOARD_COMPUTER, 7,
      control_command::kStm32Usart6ComponentId);
  mavlink_command_long_t packet{};
  mavlink_msg_command_long_decode(&message, &packet);
  CHECK(message.sysid == 7);
  CHECK(message.compid == MAV_COMP_ID_ONBOARD_COMPUTER);
  CHECK(packet.command == 31014);
  CHECK(packet.target_system == 7);
  CHECK(packet.target_component == 193);
  CHECK(packet.confirmation == 0);
  CHECK(packet.param1 == 2.0F);
  CHECK(packet.param2 == 1300.0F);
  CHECK(packet.param3 == 0.0F);
  CHECK(packet.param4 == 0.0F);
  CHECK(packet.param5 == 0.0F);
  CHECK(packet.param6 == 0.0F);
  CHECK(packet.param7 == 0.0F);
}

TEST_CASE("五模块启停命令按USART6约定编码") {
  constexpr std::array<std::pair<const char*, float>, 5> modules{{
      {"gnss", 0.0F}, {"imu", 1.0F}, {"baro", 2.0F}, {"lora", 4.0F}, {"rid", 7.0F}}};
  for (const auto& [module, wire_id] : modules) {
    for (const bool enabled : {false, true}) {
      CAPTURE(module);
      CAPTURE(enabled);
      const nlohmann::json request{{"command_id", "module-test"},
                                  {"command", "set_module_enabled"},
                                  {"parameters", {{"module", module}, {"enabled", enabled}}}};
      const auto command = control_command::Parse(request.dump());
      REQUIRE(command.has_value());
      CHECK_FALSE(control_command::IsPriorityCommand(*command));

      const auto message = control_command::EncodeCommandLong(*command, 42, 191, 42, 193);
      mavlink_command_long_t packet{};
      mavlink_msg_command_long_decode(&message, &packet);
      CHECK(message.magic == MAVLINK_STX);
      CHECK(static_cast<std::uint32_t>(message.msgid) == 76);
      CHECK(message.sysid == 42);
      CHECK(message.compid == 191);
      CHECK(packet.target_system == 42);
      CHECK(packet.target_component == 193);
      CHECK(packet.command == 31015);
      CHECK(packet.confirmation == 0);
      CHECK(packet.param1 == wire_id);
      CHECK(packet.param2 == (enabled ? 1.0F : 0.0F));
      CHECK(packet.param3 == 0.0F);
      CHECK(packet.param4 == 0.0F);
      CHECK(packet.param5 == 0.0F);
      CHECK(packet.param6 == 0.0F);
      CHECK(packet.param7 == 0.0F);
    }
  }
}

TEST_CASE("模块启停只接受指定模块和JSON布尔值") {
  for (const auto& parameters : nlohmann::json::array({
           nlohmann::json::object(), {{"module", "imu"}}, {{"enabled", true}},
           {{"module", "imu"}, {"enabled", true}, {"extra", 0}},
           {{"module", "IMU"}, {"enabled", true}},
           {{"module", "remote_id"}, {"enabled", true}},
           {{"module", "5g"}, {"enabled", true}},
           {{"module", 4}, {"enabled", true}},
           {{"module", nullptr}, {"enabled", true}},
           {{"module", "imu"}, {"enabled", "true"}},
           {{"module", "imu"}, {"enabled", "false"}},
           {{"module", "imu"}, {"enabled", 1}},
           {{"module", "imu"}, {"enabled", 0}},
           {{"module", "imu"}, {"enabled", nullptr}}})) {
    CAPTURE(parameters);
    const nlohmann::json request{{"command_id", "bad-module"},
                                {"command", "set_module_enabled"},
                                {"parameters", parameters}};
    const auto command = control_command::Parse(request.dump());
    REQUIRE_FALSE(command.has_value());
    CHECK(command.error().code == "invalid_parameter");
    CHECK(command.error().command_id == "bad-module");
    CHECK(command.error().command == "set_module_enabled");
  }
}

TEST_CASE("模块启停拒绝全空白的请求标识") {
  for (const auto id : {" \t\r\n", "\u3000 \t"}) {
    const nlohmann::json request{{"command_id", id}, {"command", "set_module_enabled"},
                                {"parameters", {{"module", "imu"}, {"enabled", true}}}};
    const auto command = control_command::Parse(request.dump());
    REQUIRE_FALSE(command.has_value());
    CHECK(command.error().code == "invalid_command_id");
  }
}

TEST_CASE("五种无参数命令采用独立命令号且全部参数为零") {
  for (const auto& [name, expected] :
       std::array<std::pair<const char*, std::uint16_t>, 5>{{
           {"arm", 31093}, {"disarm", 31094}, {"takeoff", 31091},
           {"land", 31092}, {"emergency_stop", 31090}}}) {
    CAPTURE(name);
    const nlohmann::json request{{"command_id", name}, {"command", name},
                                {"parameters", nlohmann::json::object()}};
    const auto command = control_command::Parse(request.dump());
    REQUIRE(command.has_value());
    CHECK(command->mavlink_command == expected);
    CHECK(command->params == std::array<float, 4>{});
  }
}

TEST_CASE("单电机参数允许两个边界电机编号与脉宽") {
  for (const auto motor : {1, 4}) {
    for (const auto pulse : {1000, 2000}) {
      const nlohmann::json request{
          {"command_id", "edge"}, {"command", "set_motor_pwm"},
          {"parameters", {{"motor_id", motor}, {"pwm_us", pulse}}}};
      const auto command = control_command::Parse(request.dump());
      REQUIRE(command.has_value());
      CHECK(command->params[0] == motor);
      CHECK(command->params[1] == pulse);
    }
  }
}

TEST_CASE("仅上锁与急停允许优先发送") {
  for (const auto name : {"arm", "disarm", "takeoff", "land", "emergency_stop"}) {
    const nlohmann::json request{{"command_id", name}, {"command", name},
                                {"parameters", nlohmann::json::object()}};
    const auto command = control_command::Parse(request.dump());
    REQUIRE(command.has_value());
    CHECK(control_command::IsPriorityCommand(*command) ==
          (command->command == "disarm" || command->command == "emergency_stop"));
  }
  const auto pwm = control_command::Parse(
      R"({"command_id":"pwm","command":"set_motor_pwm","parameters":{"motor_id":1,"pwm_us":1500}})");
  REQUIRE(pwm.has_value());
  CHECK_FALSE(control_command::IsPriorityCommand(*pwm));
}

TEST_CASE("单电机参数拒绝越界非整数布尔和巨大无符号数") {
  const nlohmann::json valid{
      {"command_id", "invalid"}, {"command", "set_motor_pwm"},
      {"parameters", {{"motor_id", 1}, {"pwm_us", 1500}}}};
  const auto check_values = [&](const char* key, const nlohmann::json& values) {
    for (const auto& value : values) {
      CAPTURE(key);
      CAPTURE(value);
      auto request = valid;
      request["parameters"][key] = value;
      const auto command = control_command::Parse(request.dump());
      REQUIRE_FALSE(command.has_value());
      CHECK(command.error().code == "invalid_parameter");
    }
  };
  check_values("motor_id", nlohmann::json::array(
      {0, 5, -1, 1.0, true, "1", nullptr,
       std::numeric_limits<std::uint64_t>::max()}));
  check_values("pwm_us", nlohmann::json::array(
      {999, 2001, -1, 1500.0, false, "1500", nullptr,
       std::numeric_limits<std::uint64_t>::max()}));
}

TEST_CASE("调速拒绝旧四路格式及额外参数") {
  for (const auto& parameters : nlohmann::json::array({
           {{"pwm_us", {1500, 1500, 1500, 1500}}},
           {{"motor_id", 1}},
           {{"motor_id", 1}, {"pwm_us", 1500}, {"extra", 0}}})) {
    const nlohmann::json request{{"command_id", "bad-pwm"},
                                {"command", "set_motor_pwm"},
                                {"parameters", parameters}};
    const auto command = control_command::Parse(request.dump());
    REQUIRE_FALSE(command.has_value());
    CHECK(command.error().code == "invalid_parameter");
    CHECK(command.error().command_id == "bad-pwm");
  }
}

TEST_CASE("parameters字段必须显式提供对象") {
  CHECK_FALSE(control_command::Parse(
      R"({"command_id":"missing","command":"takeoff"})").has_value());
  for (const auto& parameters : nlohmann::json::array(
           {nullptr, true, 1, "", nlohmann::json::array()})) {
    const nlohmann::json request{{"command_id", "invalid"},
                                {"command", "takeoff"},
                                {"parameters", parameters}};
    CHECK_FALSE(control_command::Parse(request.dump()).has_value());
  }
}

TEST_CASE("无参数命令拒绝多余参数及旧命令名") {
  for (const auto name : {"arm", "disarm", "takeoff", "land", "emergency_stop"}) {
    const nlohmann::json request{{"command_id", "extra"}, {"command", name},
                                {"parameters", {{"height", 10}}}};
    CHECK_FALSE(control_command::Parse(request.dump()).has_value());
  }
  for (const auto name : {"unlock", "lock", "hover"}) {
    const nlohmann::json request{{"command_id", "unknown"}, {"command", name},
                                {"parameters", nlohmann::json::object()}};
    const auto command = control_command::Parse(request.dump());
    REQUIRE_FALSE(command.has_value());
    CHECK(command.error().code == "unsupported_command");
  }
}

TEST_CASE("命令标识必须非空且不超过128字节") {
  for (const auto& id : {std::string{}, std::string(129, 'x')}) {
    const nlohmann::json request{{"command_id", id}, {"command", "arm"},
                                {"parameters", nlohmann::json::object()}};
    const auto command = control_command::Parse(request.dump());
    REQUIRE_FALSE(command.has_value());
    CHECK(command.error().code == "invalid_command_id");
  }
  const nlohmann::json request{{"command_id", std::string(128, 'x')},
                              {"command", "arm"},
                              {"parameters", nlohmann::json::object()}};
  CHECK(control_command::Parse(request.dump()).has_value());
}

TEST_CASE("解析失败保留可用的命令关联信息") {
  const auto invalid_parameters = control_command::Parse(
      R"({"command_id":"pwm-invalid","command":"set_motor_pwm","parameters":{"motor_id":0,"pwm_us":1500}})");
  REQUIRE_FALSE(invalid_parameters.has_value());
  CHECK(invalid_parameters.error().command_id == "pwm-invalid");
  CHECK(invalid_parameters.error().command == "set_motor_pwm");

  const auto invalid_command = control_command::Parse(
      R"({"command_id":"bad-command","command":42,"parameters":{}})");
  REQUIRE_FALSE(invalid_command.has_value());
  CHECK(invalid_command.error().command_id == "bad-command");
  CHECK(invalid_command.error().command.empty());
}

TEST_CASE("受理成功与等待回执不声称动作已经完成") {
  const auto command = control_command::Parse(
      R"({"command_id":"flight-001","command":"takeoff","parameters":{}})");
  REQUIRE(command.has_value());
  const auto pending = control_command::BuildPendingAck(*command);
  CHECK(pending["status"] == "in_progress");
  CHECK(pending["result_code"] == "pending");
  CHECK(pending["message"] == "命令已下发，等待F407受理");
  const auto accepted = control_command::BuildMavlinkAck(*command, MAV_RESULT_ACCEPTED);
  CHECK(accepted["status"] == "accepted");
  CHECK(accepted["message"] == "F407已接受命令");
  CHECK_FALSE(accepted.contains("error_code"));
}

TEST_CASE("F407拒绝细分原因转换为软件错误码") {
  const auto command = control_command::Parse(
      R"({"command_id":"flight-001","command":"takeoff","parameters":{}})");
  REQUIRE(command.has_value());
  constexpr std::array<const char*, 5> codes{
      "device_locked", "device_busy", "invalid_parameter", "invalid_state",
      "device_check_failed"};
  constexpr std::array<std::uint8_t, 5> results{2, 1, 2, 2, 4};
  for (std::size_t index = 0; index < codes.size(); ++index) {
    const auto ack = control_command::BuildMavlinkAck(
        *command, results[index], 255, static_cast<std::int32_t>(index + 1));
    CHECK(ack["status"] == "rejected");
    CHECK(ack["error_code"] == codes[index]);
    CHECK(ack["result"] == results[index]);
  }
}

TEST_CASE("未细分的F407拒绝提供稳定错误码") {
  const auto command = control_command::Parse(
      R"({"command_id":"flight-001","command":"takeoff","parameters":{}})");
  REQUIRE(command.has_value());
  for (const auto result : {MAV_RESULT_TEMPORARILY_REJECTED, MAV_RESULT_DENIED,
                            MAV_RESULT_FAILED}) {
    for (const auto reason : {0, -1, 99}) {
      const auto rejected = control_command::BuildMavlinkAck(
          *command, result, 255, reason);
      CHECK(rejected["status"] == "rejected");
      CHECK(rejected.value("error_code", "") == "mcu_rejected");
      CHECK(rejected["message"] == "F407拒绝命令");
    }
  }
  const auto unsupported = control_command::BuildMavlinkAck(
      *command, MAV_RESULT_UNSUPPORTED, 255, 0);
  CHECK(unsupported.value("error_code", "") == "unsupported_command");
  CHECK(unsupported["message"] == "F407不支持该命令");

  const auto detailed = control_command::BuildMavlinkAck(
      *command, MAV_RESULT_UNSUPPORTED, 255, 2);
  CHECK(detailed["error_code"] == "device_busy");
  CHECK(detailed["message"] == "设备正在执行其他动作");
}

TEST_CASE("受理超时明确表示执行状态未知") {
  const auto command = control_command::Parse(
      R"({"command_id":"flight-001","command":"takeoff","parameters":{}})");
  REQUIRE(command.has_value());
  const auto timeout = control_command::BuildTimeoutAck(*command);
  CHECK(timeout["status"] == "timeout");
  CHECK(timeout["error_code"] == "mcu_ack_timeout");
  CHECK(timeout["message"] == "未收到F407受理结果，执行状态未知");
}
