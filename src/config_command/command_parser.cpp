/**
 * @file command_parser.cpp
 * @brief command_parser.hpp 的实现。
 */

#include "config_command/command_parser.hpp"

#include <array>
#include <limits>

namespace config_command {
namespace {

CommandError Error(std::string code, std::string message) {
  return {.code = std::move(code), .message = std::move(message)};
}

std::expected<int, CommandError> ReadInteger(const nlohmann::json& value,
                                             std::string_view name, int minimum,
                                             int maximum) {
  if (!value.is_number_integer() && !value.is_number_unsigned()) {
    return std::unexpected(
        Error("invalid_parameter_type", std::string(name) + "必须是整数"));
  }
  std::int64_t number = 0;
  if (value.is_number_unsigned()) {
    const auto unsigned_number = value.get<std::uint64_t>();
    if (unsigned_number > static_cast<std::uint64_t>(maximum)) {
      return std::unexpected(
          Error("invalid_parameter", std::string(name) + "超出允许范围"));
    }
    number = static_cast<std::int64_t>(unsigned_number);
  } else {
    number = value.get<std::int64_t>();
  }
  if (number < minimum || number > maximum) {
    return std::unexpected(
        Error("invalid_parameter", std::string(name) + "超出允许范围"));
  }
  return static_cast<int>(number);
}

bool IsAllowedParameter(std::string_view name) {
  constexpr std::array<std::string_view, 4> allowed = {
      "telemetry_publish_interval_ms", "heartbeat_interval_ms",
      "mqtt_reconnect_delay_s", "mqtt_reconnect_delay_max_s"};
  for (const auto candidate : allowed) {
    if (candidate == name) return true;
  }
  return false;
}

}  // namespace

std::expected<void, CommandError> ValidateConfigParameterPatch(
    const ConfigParameterPatch& parameters) {
  const auto valid = [](const std::optional<int>& value, int minimum, int maximum) {
    return !value || (*value >= minimum && *value <= maximum);
  };
  if (!valid(parameters.telemetry_publish_interval_ms, 100, 60000) ||
      !valid(parameters.heartbeat_interval_ms, 100, 60000) ||
      !valid(parameters.mqtt_reconnect_delay_s, 1, 3600) ||
      !valid(parameters.mqtt_reconnect_delay_max_s, 1, 3600)) {
    return std::unexpected(Error("invalid_parameter", "配置参数超出允许范围"));
  }
  if (parameters.mqtt_reconnect_delay_s && parameters.mqtt_reconnect_delay_max_s &&
      *parameters.mqtt_reconnect_delay_s > *parameters.mqtt_reconnect_delay_max_s) {
    return std::unexpected(
        Error("invalid_parameter", "mqtt_reconnect_delay_s不能大于最大等待"));
  }
  return {};
}

std::expected<ConfigCommand, CommandError> ParseConfigCommand(std::string_view payload) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(payload);
  } catch (const nlohmann::json::parse_error&) {
    return std::unexpected(Error("invalid_json", "命令不是合法JSON"));
  }

  if (!root.is_object() || !root.contains("command_id") ||
      !root["command_id"].is_string()) {
    return std::unexpected(Error("invalid_command_id", "command_id缺失或类型非法"));
  }
  const auto command_id = root["command_id"].get<std::string>();
  if (command_id.empty() || command_id.size() > 128) {
    return std::unexpected(Error("invalid_command_id", "command_id长度非法"));
  }
  if (!root.contains("parameters") || !root["parameters"].is_object() ||
      root["parameters"].empty()) {
    return std::unexpected(Error("invalid_parameter", "parameters必须是非空对象"));
  }
  for (auto it = root.begin(); it != root.end(); ++it) {
    if (it.key() != "command_id" && it.key() != "parameters") {
      return std::unexpected(
          Error("unknown_parameter", it.key() + "不是允许的命令字段"));
    }
  }

  ConfigParameterPatch patch;
  const auto& parameters = root["parameters"];
  for (auto it = parameters.begin(); it != parameters.end(); ++it) {
    if (!IsAllowedParameter(it.key())) {
      return std::unexpected(Error("unknown_parameter", it.key() + "不允许远程修改"));
    }
    const bool milliseconds = it.key() == "telemetry_publish_interval_ms" ||
                              it.key() == "heartbeat_interval_ms";
    auto number = ReadInteger(it.value(), it.key(), milliseconds ? 100 : 1,
                              milliseconds ? 60000 : 3600);
    if (!number) return std::unexpected(number.error());
    if (it.key() == "telemetry_publish_interval_ms") {
      patch.telemetry_publish_interval_ms = *number;
    } else if (it.key() == "heartbeat_interval_ms") {
      patch.heartbeat_interval_ms = *number;
    } else if (it.key() == "mqtt_reconnect_delay_s") {
      patch.mqtt_reconnect_delay_s = *number;
    } else {
      patch.mqtt_reconnect_delay_max_s = *number;
    }
  }

  auto validation = ValidateConfigParameterPatch(patch);
  if (!validation) return std::unexpected(validation.error());
  return ConfigCommand{.command_id = command_id, .parameters = patch};
}

nlohmann::json BuildRejectedAck(std::string_view command_id, const CommandError& error) {
  return {{"command_id", command_id},
          {"status", "rejected"},
          {"error_code", error.code},
          {"message", error.message},
          {"restart_required", false}};
}

}  // namespace config_command
