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

bool IsReportTopic(std::string_view topic, const ReportTopicNames& names) {
  return topic == names.snapshot || topic == names.realtime ||
         topic == "runtime/status/v1" || topic == "imu/sample/v1" ||
         topic == "gnss/raw/v1";
}

}  // namespace

std::expected<void, CommandError> ValidateConfigParameterPatch(
    const ConfigParameterPatch& parameters, const ReportTopicNames& topics) {
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
  if (!parameters.topic_publish_intervals_ms.empty() &&
      (parameters.telemetry_publish_interval_ms || parameters.heartbeat_interval_ms ||
       parameters.mqtt_reconnect_delay_s || parameters.mqtt_reconnect_delay_max_s)) {
    return std::unexpected(Error("invalid_parameter", "Topic调频须与重启参数分开下发"));
  }
  for (const auto& [topic, interval] : parameters.topic_publish_intervals_ms) {
    if (!IsReportTopic(topic, topics)) {
      return std::unexpected(Error("unknown_parameter", topic + "不是数据上报Topic"));
    }
    if (!((interval >= 100 && interval <= 60000) ||
          (topic == "imu/sample/v1" && interval == 0))) {
      return std::unexpected(Error("invalid_parameter", topic + "周期超出允许范围"));
    }
  }
  return {};
}

std::expected<ConfigCommand, CommandError> ParseConfigCommand(
    std::string_view payload, const ReportTopicNames& topics) {
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
  if (parameters.contains("topic_publish_intervals_ms")) {
    const auto& intervals = parameters["topic_publish_intervals_ms"];
    if (parameters.size() != 1 || !intervals.is_object() || intervals.empty()) {
      return std::unexpected(Error("invalid_parameter", "Topic周期须为非空对象且单独下发"));
    }
    for (auto it = intervals.begin(); it != intervals.end(); ++it) {
      if (!IsReportTopic(it.key(), topics)) {
        return std::unexpected(Error("unknown_parameter", it.key() + "不是数据上报Topic"));
      }
      auto interval = ReadInteger(it.value(), it.key(), it.key() == "imu/sample/v1" ? 0 : 100, 60000);
      if (!interval) return std::unexpected(interval.error());
      if (*interval != 0 && *interval < 100) {
        return std::unexpected(Error("invalid_parameter", "周期必须为100至60000毫秒"));
      }
      patch.topic_publish_intervals_ms.emplace(it.key(), *interval);
    }
    return ConfigCommand{.command_id = command_id, .parameters = std::move(patch),
                         .report_topics = topics};
  }
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

  auto validation = ValidateConfigParameterPatch(patch, topics);
  if (!validation) return std::unexpected(validation.error());
  return ConfigCommand{.command_id = command_id, .parameters = patch, .report_topics = topics};
}

nlohmann::json BuildRejectedAck(std::string_view command_id, const CommandError& error) {
  return {{"command_id", command_id},
          {"status", "rejected"},
          {"error_code", error.code},
          {"message", error.message},
          {"restart_required", false}};
}

}  // namespace config_command
