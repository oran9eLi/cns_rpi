#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "control_command/control_command.hpp"
#include "control_test/control_test_cli.hpp"
#include "uart/serial_port.hpp"

TEST_CASE("直接命令内容默认仅解析而不发送") {
  const std::array<std::string_view, 2> args{
      "config/config.json",
      R"({"command_id":"local-1","command":"takeoff","parameters":{}})"};
  auto result = control_test::ParseArguments(args);
  REQUIRE(result.has_value());
  CHECK_FALSE(result->send);
  CHECK(result->config_path == "config/config.json");
  REQUIRE(result->inline_payload.has_value());
}

TEST_CASE("发送选项和文件选项的位置不受限制") {
  const std::array<std::string_view, 4> args{
      "--file", "/tmp/control.json", "--send", "config/config.json"};
  auto result = control_test::ParseArguments(args);
  REQUIRE(result.has_value());
  CHECK(result->send);
  CHECK(result->payload_file == "/tmp/control.json");
}

TEST_CASE("直接命令内容和命令文件互斥") {
  const std::array<std::string_view, 4> args{
      "config/config.json", "{}", "--file", "/tmp/control.json"};
  auto result = control_test::ParseArguments(args);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == "conflicting_payload_sources");
}

TEST_CASE("拒绝缺少配置路径或命令内容") {
  SUBCASE("没有参数") {
    const std::array<std::string_view, 0> args{};
    auto result = control_test::ParseArguments(args);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "missing_config_path");
  }
  SUBCASE("只有配置路径") {
    const std::array<std::string_view, 1> args{"config/config.json"};
    auto result = control_test::ParseArguments(args);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "missing_payload");
  }
}

TEST_CASE("拒绝无值的文件选项、未知选项和重复发送选项") {
  SUBCASE("文件选项没有路径") {
    const std::array<std::string_view, 1> args{"--file"};
    auto result = control_test::ParseArguments(args);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "missing_option_value");
  }
  SUBCASE("未知选项") {
    const std::array<std::string_view, 3> args{
        "--unknown", "config/config.json", "{}"};
    auto result = control_test::ParseArguments(args);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "unknown_option");
  }
  SUBCASE("重复发送选项") {
    const std::array<std::string_view, 4> args{
        "--send", "config/config.json", "{}", "--send"};
    auto result = control_test::ParseArguments(args);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "duplicate_option");
  }
}

TEST_CASE("拒绝两个普通命令内容参数") {
  const std::array<std::string_view, 3> args{
      "config/config.json", "{}", "{}"};
  auto result = control_test::ParseArguments(args);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == "too_many_arguments");
}

TEST_CASE("读取直接命令内容和命令文件") {
  SUBCASE("直接命令内容原样返回") {
    control_test::CliOptions options;
    options.inline_payload = "{}";
    auto result = control_test::LoadPayload(options);
    REQUIRE(result.has_value());
    CHECK(*result == "{}");
  }

  SUBCASE("空文件被拒绝") {
    const auto path = std::filesystem::temp_directory_path() /
                      "cns_control_test_empty_payload.json";
    {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      REQUIRE(output.good());
    }
    control_test::CliOptions options;
    options.payload_file = path;
    const auto result = control_test::LoadPayload(options);
    std::filesystem::remove(path);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "empty_payload");
  }

  SUBCASE("文件内容完整读取") {
    const auto path = std::filesystem::temp_directory_path() /
                      "cns_control_test_payload.json";
    {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      REQUIRE(output.good());
      output << "{\n  \"command\": \"land\"\n}";
    }
    control_test::CliOptions options;
    options.payload_file = path;
    const auto result = control_test::LoadPayload(options);
    std::filesystem::remove(path);
    REQUIRE(result.has_value());
    CHECK(*result == "{\n  \"command\": \"land\"\n}");
  }

  SUBCASE("打不开文件被拒绝") {
    control_test::CliOptions options;
    options.payload_file = "/tmp/cns_control_test_missing_payload.json";
    const auto result = control_test::LoadPayload(options);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "payload_file_unreadable");
  }
}

TEST_CASE("空运行输出实际MAVLink映射但不伪造目标系统编号") {
  auto command = control_command::Parse(
      R"({"command_id":"local-1","command":"set_motor_pwm","parameters":{"pwm_us":[1500,1510,1520,1530]}})");
  REQUIRE(command.has_value());

  const auto result = control_test::BuildDryRunResult(*command);

  CHECK(result["status"] == "dry_run");
  CHECK(result["command_id"] == "local-1");
  CHECK(result["mavlink_command"] == control_command::kSetMotorPwmUs);
  CHECK(result["target_system"] == nullptr);
  CHECK(result["target_component"] == 193);
  CHECK(result["params"] ==
        nlohmann::json::array({1500, 1510, 1520, 1530, 0, 0, 0}));
}

TEST_CASE("最终回执只在接受时返回成功退出码") {
  CHECK(control_test::ExitCodeForFinalAck({{"status", "accepted"}}) == 0);
  CHECK(control_test::ExitCodeForFinalAck({{"status", "rejected"}}) == 1);
  CHECK(control_test::ExitCodeForFinalAck({{"status", "timeout"}}) == 1);
  CHECK(control_test::ExitCodeForFinalAck(nlohmann::json::object()) == 1);
}

TEST_CASE("串口写入失败时即使输出拒绝回执也返回串口错误退出码") {
  CHECK(control_test::ExitCodeForFinalAck({{"status", "rejected"}}, true) == 3);
}

TEST_CASE("串口被占用时诊断信息明确提示停止主服务") {
  const auto message = control_test::UartOpenErrorMessage(
      uart::UartError::kDeviceBusy, "/dev/ttyUSB0");

  CHECK(message.find("串口已被占用") != std::string::npos);
  CHECK(message.find("systemctl stop cns-rpi.service") != std::string::npos);
}
