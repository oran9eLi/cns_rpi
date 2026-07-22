#pragma once

#include "config/app_config.hpp"

#include <string>

namespace config {

/// 构造主程序四个运行期可配置参数的中文单行摘要。
std::string BuildStartupSummary(const AppConfig& config);

}  // namespace config
