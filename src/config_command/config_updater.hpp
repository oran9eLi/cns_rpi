#pragma once

/**
 * @file config_updater.hpp
 * @brief 在完整原始JSON副本上应用白名单补丁，避免丢失未知配置字段。
 */

#include <expected>
#include <filesystem>
#include <string_view>

#include <nlohmann/json.hpp>

#include "config_command/command_parser.hpp"

namespace config { struct AppConfig; }

namespace config_command {

std::expected<nlohmann::json, CommandError> LoadConfigJson(
    const std::filesystem::path& path);
bool IsCommandApplied(const nlohmann::json& root, std::string_view command_id);
ReportTopicNames ReportTopics(const nlohmann::json& config);
nlohmann::json ReportIntervals(const nlohmann::json& config);
void ApplyReportIntervals(config::AppConfig& config, const ConfigParameterPatch& patch);
std::expected<nlohmann::json, CommandError> BuildUpdatedConfig(
    const nlohmann::json& current, const ConfigCommand& command);

}  // namespace config_command
