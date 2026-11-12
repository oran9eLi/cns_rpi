#pragma once

/**
 * @file command_parser.hpp
 * @brief 解析运行参数和数据Topic周期补丁。
 */

#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace config_command {

struct ConfigParameterPatch {
  std::optional<int> telemetry_publish_interval_ms;
  std::optional<int> heartbeat_interval_ms;
  std::optional<int> mqtt_reconnect_delay_s;
  std::optional<int> mqtt_reconnect_delay_max_s;
  std::map<std::string, int> topic_publish_intervals_ms;
};

struct ReportTopicNames {
  std::string snapshot{"telemetry/snapshot/v1"};
  std::string realtime{"telemetry/realtime/v1"};
  bool operator==(const ReportTopicNames&) const = default;
};

struct ConfigCommand {
  std::string command_id;
  ConfigParameterPatch parameters;
  ReportTopicNames report_topics{};
};

struct CommandError {
  std::string code;
  std::string message;
};

std::expected<void, CommandError> ValidateConfigParameterPatch(
    const ConfigParameterPatch& parameters, const ReportTopicNames& topics = {});
std::expected<ConfigCommand, CommandError> ParseConfigCommand(
    std::string_view payload, const ReportTopicNames& topics = {});
nlohmann::json BuildRejectedAck(std::string_view command_id, const CommandError& error);

}  // namespace config_command
