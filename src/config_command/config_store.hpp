#pragma once

/**
 * @file config_store.hpp
 * @brief 按可信启动参数选择配置写入后端，远程命令不能改变该选择。
 */

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "config_command/command_parser.hpp"

namespace config_command {

enum class ConfigWriterMode { kDisabled, kDirect, kHelper };

struct WriterOptions {
  ConfigWriterMode mode = ConfigWriterMode::kDisabled;
  std::filesystem::path helper_path;
};

std::expected<WriterOptions, std::string> ParseWriterOptions(
    const std::vector<std::string_view>& arguments);
std::expected<void, CommandError> PersistConfig(
    const WriterOptions& options, const std::filesystem::path& config_path,
    const nlohmann::json& candidate);

}  // namespace config_command
