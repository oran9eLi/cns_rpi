#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

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

TEST_CASE("主程序只编排Logger且不记录原始遥测JSON") {
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
  CHECK(text.find("LogPublishedTelemetry") == std::string::npos);
  CHECK(text.find("BuildStartupSummary(*app_config)") != std::string::npos);
  CHECK(text.find("cns_rpi M3c 启动") == std::string::npos);

  CHECK(text.find("帧只更新 state::StateStore") != std::string::npos);
  CHECK(text.find("日志格式和输出目标由 logging::Logger 独立负责") != std::string::npos);
  CHECK(text.find("不把遥测 JSON 写入运行日志") != std::string::npos);

  const auto logger_declaration = text.find("auto logger = logging::Logger::Create(");
  const auto mqtt_declaration = text.find("std::optional<mqtt::MqttClient> mqtt_client;");
  REQUIRE(logger_declaration != std::string::npos);
  REQUIRE(mqtt_declaration != std::string::npos);
  CHECK(logger_declaration < mqtt_declaration);
  CHECK(text.find("}, **logger);") != std::string::npos);

  CHECK(text.find("std::optional<telemetry::Publisher> telemetry_publisher") !=
        std::string::npos);
  CHECK(text.find("payload::ToRealtimeJson") != std::string::npos);
  CHECK(text.find("telemetry_publisher->Tick") != std::string::npos);
  CHECK(text.find("遥测快照通道") != std::string::npos);
  CHECK(text.find("遥测实时通道") != std::string::npos);
  CHECK(text.find("PX4实时遥测已启用") == std::string::npos);
  CHECK(text.find("last_px4_realtime_publish") == std::string::npos);

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
  CHECK(text.find("已重新发现受控设备 MAVLink 串口，链路恢复") !=
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
  // 收到当前受控设备的 COMMAND_ACK 应答，且日志取自事务处理结果而非丢弃。
  const auto ack_branch = Between(text, "IsExpectedCommandAck(",
                                  "const bool decoded_standard");
  CHECK(ack_branch.find("收到设备应答") != std::string_view::npos);
  CHECK(ack_branch.find("control_command::ResultCode(ack.result)") != std::string_view::npos);
  // 应答处理结果必须被取用（用于区分最终/进行中/未匹配），不能再退回 (void) 丢弃。
  CHECK(ack_branch.find("(void)control_transaction.HandleMavlinkAck") == std::string_view::npos);
}

TEST_CASE("主程序使用运行三态遗嘱并以非阻塞方式事件发布") {
  const auto main_path = std::filesystem::path(SOURCE_DIR) / "src/main.cpp";
  std::ifstream input(main_path);
  REQUIRE(input.is_open());
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};

  CHECK(text.find("BuildLastWillPublication(") != std::string::npos);
  CHECK(text.find("topics.runtime_status.suffix") != std::string::npos);
  CHECK(text.find("runtime_status::PublicationState") != std::string::npos);
  CHECK(text.find("runtime_publication_state.ShouldPublish(") !=
        std::string::npos);
  CHECK(text.find("mqtt_client->ConnectionGeneration()") !=
        std::string::npos);
  CHECK(text.find("mqtt_will_has_persisted_binding") !=
        std::string::npos);
  CHECK(text.find("CanAcceptTelemetryMessage(") !=
        std::string::npos);

  const auto publish_branch = Between(
      text, "// 运行三态发布开始", "// 运行三态发布结束");
  CHECK(publish_branch.find("mqtt_client->Publish(") !=
        std::string_view::npos);
  CHECK(publish_branch.find("PublishAndWait") == std::string_view::npos);
  CHECK(publish_branch.find("return ") == std::string_view::npos);
  CHECK(publish_branch.find("break;") == std::string_view::npos);
  CHECK(publish_branch.find("close_mqtt_session") == std::string_view::npos);
  CHECK(publish_branch.find("publication->payload);") ==
        std::string_view::npos);
  CHECK(text.find("MQTT运行三态发布失败，连接恢复后重试") !=
        std::string::npos);
}

TEST_CASE("实验ACK等待QoS完成回调才从待发队列移除") {
  const auto main_path = std::filesystem::path(SOURCE_DIR) / "src/main.cpp";
  std::ifstream input(main_path);
  REQUIRE(input.is_open());
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  CHECK(text.find("mqtt_client->PublishTracked(") != std::string::npos);
  CHECK(text.find("mqtt_client->TakePublishCompletion(") != std::string::npos);
  CHECK(text.find("experiment_ack_outbox.ConfirmFront();") != std::string::npos);
  CHECK(text.find("mqtt_client->ForgetTrackedPublish(") != std::string::npos);
}
