/**
 * @file config_updater.cpp
 * @brief config_updater.hpp 的实现。
 */

#include "config_command/config_updater.hpp"

#include <fstream>

namespace config_command {
namespace {
CommandError InvalidCurrentConfig() {
  return {.code = "config_read_failed", .message = "当前配置结构非法"};
}
}  // namespace

std::expected<nlohmann::json, CommandError> LoadConfigJson(
    const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    return std::unexpected(
        CommandError{.code = "config_read_failed", .message = "无法读取当前配置"});
  }
  try {
    nlohmann::json root;
    input >> root;
    if (!root.is_object()) return std::unexpected(InvalidCurrentConfig());
    return root;
  } catch (const nlohmann::json::exception&) {
    return std::unexpected(InvalidCurrentConfig());
  }
}

bool IsCommandApplied(const nlohmann::json& root, std::string_view command_id) {
  try {
    const auto& ids = root.at("runtime").at("applied_command_ids");
    if (!ids.is_array()) return false;
    for (const auto& id : ids) {
      if (id.is_string() && id.get_ref<const std::string&>() == command_id) return true;
    }
  } catch (const nlohmann::json::exception&) {
    return false;
  }
  return false;
}

std::expected<nlohmann::json, CommandError> BuildUpdatedConfig(
    const nlohmann::json& current, const ConfigCommand& command) {
  try {
    nlohmann::json candidate = current;
    auto& runtime = candidate.at("runtime");
    auto& reconnect = candidate.at("mqtt").at("connection").at("reconnect");
    auto& applied_ids = runtime.at("applied_command_ids");
    if (!runtime.is_object() || !reconnect.is_object() || !applied_ids.is_array()) {
      return std::unexpected(InvalidCurrentConfig());
    }

    const auto& patch = command.parameters;
    if (patch.telemetry_publish_interval_ms) {
      runtime["telemetry_publish_interval_ms"] = *patch.telemetry_publish_interval_ms;
    }
    if (patch.heartbeat_interval_ms) {
      runtime["heartbeat_interval_ms"] = *patch.heartbeat_interval_ms;
    }
    if (patch.mqtt_reconnect_delay_s) {
      reconnect["delay_s"] = *patch.mqtt_reconnect_delay_s;
    }
    if (patch.mqtt_reconnect_delay_max_s) {
      reconnect["delay_max_s"] = *patch.mqtt_reconnect_delay_max_s;
    }

    if (!reconnect.at("delay_s").is_number_integer() ||
        !reconnect.at("delay_max_s").is_number_integer() ||
        reconnect.at("delay_s").get<int>() > reconnect.at("delay_max_s").get<int>()) {
      return std::unexpected(CommandError{
          .code = "invalid_parameter",
          .message = "mqtt_reconnect_delay_s不能大于最大等待",
      });
    }

    applied_ids.push_back(command.command_id);
    while (applied_ids.size() > 32) applied_ids.erase(applied_ids.begin());
    return candidate;
  } catch (const nlohmann::json::exception&) {
    return std::unexpected(InvalidCurrentConfig());
  }
}

}  // namespace config_command
