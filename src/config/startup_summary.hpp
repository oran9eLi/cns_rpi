#pragma once

#include "config/app_config.hpp"

#include <filesystem>
#include <string>

namespace config {

/// 构造主程序实际生效的单行配置摘要；认证字段只输出是否配置。
std::string BuildStartupSummary(const AppConfig& config,
                                const std::filesystem::path& config_path);

}  // namespace config
