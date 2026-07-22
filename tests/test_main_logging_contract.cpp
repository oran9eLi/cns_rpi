#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

std::size_t Count(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t position = 0;
       (position = text.find(needle, position)) != std::string_view::npos;
       position += needle.size()) {
    ++count;
  }
  return count;
}

std::string_view Between(std::string_view text, std::string_view begin,
                         std::string_view end) {
  const auto begin_position = text.find(begin);
  REQUIRE(begin_position != std::string_view::npos);
  const auto content_position = begin_position + begin.size();
  const auto end_position = text.find(end, content_position);
  REQUIRE(end_position != std::string_view::npos);
  return text.substr(content_position, end_position - content_position);
}

}  // namespace

TEST_CASE("主程序只编排Logger并在遥测发布成功后记录同一JSON") {
  const auto main_path = std::filesystem::path(SOURCE_DIR) / "src/main.cpp";
  std::ifstream input(main_path);
  REQUIRE(input.is_open());
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};

  CHECK(text.find("logging/logger.hpp") != std::string::npos);
  CHECK(text.find("void LogTelemetry(") == std::string::npos);
  CHECK(text.find("void LogExtension(") == std::string::npos);
  CHECK(text.find("void LogJsonPayload(") == std::string::npos);
  CHECK(text.find("dump(2)") == std::string::npos);
  CHECK(text.find("LogPublishedTelemetry") != std::string::npos);
  CHECK(text.find("BuildStartupSummary(*app_config, config_path)") != std::string::npos);
  CHECK(text.find("cns_rpi M3c 启动") == std::string::npos);

  CHECK(text.find("帧只更新 state::StateStore") != std::string::npos);
  CHECK(text.find("日志格式和输出目标由 logging::Logger 独立负责") != std::string::npos);
  CHECK(text.find("Publish 成功后记录同一份紧凑 JSON") != std::string::npos);

  const auto logger_declaration = text.find("auto logger = logging::Logger::Create(");
  const auto mqtt_declaration = text.find("std::optional<mqtt::MqttClient> mqtt_client;");
  REQUIRE(logger_declaration != std::string::npos);
  REQUIRE(mqtt_declaration != std::string::npos);
  CHECK(logger_declaration < mqtt_declaration);
  CHECK(text.find("}, **logger);") != std::string::npos);

  CHECK(Count(text, "payload::ToJson(") == 1);
  CHECK(Count(text, ").dump();") == 1);
  const auto telemetry_branch = Between(text, "const std::string json_str =",
                                        "last_telemetry_publish = now;");
  CHECK(telemetry_branch.find("Publish(telemetry_topic, json_str,") !=
        std::string_view::npos);
  // 遥测是按节拍刷新的实时值，retained 会让新订阅者把掉电前的陈旧快照当成实时数据；
  // 设备在线与否由 registration topic 的 retained online/offline 表达。
  CHECK(telemetry_branch.find("/*retain=*/false") != std::string_view::npos);
  CHECK(telemetry_branch.find("/*retain=*/true") == std::string_view::npos);
  const auto publish_success = telemetry_branch.find(")) {");
  const auto publish_failure = telemetry_branch.find("} else {");
  REQUIRE(publish_success != std::string_view::npos);
  REQUIRE(publish_failure != std::string_view::npos);
  CHECK(telemetry_branch.find("LogPublishedTelemetry(**logger, json_str)", publish_success) <
        publish_failure);
  const auto failure_branch = telemetry_branch.substr(publish_failure);
  CHECK(failure_branch.find("Warn(\"MQTT发布失败，下个节拍重试\")") !=
        std::string_view::npos);
  CHECK(failure_branch.find("json_str") == std::string_view::npos);
  CHECK(failure_branch.find("LogPublishedTelemetry") == std::string_view::npos);

  const auto configured_business_code = Between(
      text, "if (!logger) {", "return EXIT_SUCCESS;");
  const auto logger_initialization_end = configured_business_code.find("}\n\n");
  REQUIRE(logger_initialization_end != std::string_view::npos);
  const auto after_logger_initialization =
      configured_business_code.substr(logger_initialization_end + 3);
  CHECK(after_logger_initialization.find("std::cout") == std::string_view::npos);
  CHECK(after_logger_initialization.find("std::cerr") == std::string_view::npos);
}

TEST_CASE("主程序后台发现串口并保留失败与恢复诊断") {
  const auto main_path = std::filesystem::path(SOURCE_DIR) / "src/main.cpp";
  std::ifstream input(main_path);
  REQUIRE(input.is_open());
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};

  CHECK(text.find("uart::AsyncMavlinkDiscovery") != std::string::npos);
  CHECK(text.find("discovery.Start(") != std::string::npos);
  CHECK(text.find("discovery.TryTakeResult()") != std::string::npos);
  CHECK(text.find("uart::FormatCandidateFailures(attempt->failures)") !=
        std::string::npos);
  CHECK(text.find("已重新发现STM32 MAVLink串口，链路恢复") !=
        std::string::npos);
  CHECK(text.find("uart::DiscoverMavlinkPortOnce(") == std::string::npos);
}

TEST_CASE("主程序在收到命令与应答时记录日志") {
  const auto main_path = std::filesystem::path(SOURCE_DIR) / "src/main.cpp";
  std::ifstream input(main_path);
  REQUIRE(input.is_open());
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};

  // 收到配置命令与回执。
  CHECK(text.find("收到配置命令: command_id=") != std::string::npos);
  CHECK(text.find("配置命令回执: command_id=") != std::string::npos);
  // 收到飞控命令。
  CHECK(text.find("收到飞控命令: command_id=") != std::string::npos);
  // 收到 STM32 的 COMMAND_ACK 应答，且日志取自事务处理结果而非丢弃。
  const auto ack_branch = Between(text, "IsExpectedCommandAck(", "state_store.UpdateDcdwLabel");
  CHECK(ack_branch.find("收到STM32应答") != std::string_view::npos);
  CHECK(ack_branch.find("control_command::ResultCode(ack.result)") != std::string_view::npos);
  // 应答处理结果必须被取用（用于区分最终/进行中/未匹配），不能再退回 (void) 丢弃。
  CHECK(ack_branch.find("(void)control_transaction.HandleMavlinkAck") == std::string_view::npos);
}
