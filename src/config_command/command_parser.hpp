#pragma once

/**
 * @file command_parser.hpp
 * @brief 解析服务器规范化的配置命令，只接受设计文档规定的四个白名单参数。
 */

#include <expected>
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
};

struct ConfigCommand {
  std::string command_id;
  ConfigParameterPatch parameters;
};

struct CommandError {
  std::string code;
  std::string message;
};

std::expected<void, CommandError> ValidateConfigParameterPatch(
    const ConfigParameterPatch& parameters);
std::expected<ConfigCommand, CommandError> ParseConfigCommand(std::string_view payload);
nlohmann::json BuildRejectedAck(std::string_view command_id, const CommandError& error);

}  // namespace config_command
