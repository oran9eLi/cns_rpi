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
  /// helper 模式下候选文件的暂存目录；留空表示沿用配置文件所在目录。
  /// 只读根文件系统方案里配置目录本身是只读挂载，候选不能建在那里，必须由
  /// 启动参数显式指定一个可写目录（部署时用 systemd RuntimeDirectory= 提供的
  /// tmpfs 目录）。不设默认值是刻意的：机器相关的绝对路径不该写死在类型里。
  std::filesystem::path staging_directory{};
};

std::expected<WriterOptions, std::string> ParseWriterOptions(
    const std::vector<std::string_view>& arguments);
std::expected<void, CommandError> PersistConfig(
    const WriterOptions& options, const std::filesystem::path& config_path,
    const nlohmann::json& candidate);

}  // namespace config_command
