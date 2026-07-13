#pragma once

/**
 * @file command_processor.hpp
 * @brief 编排配置命令的解析、去重、候选更新和持久化，不直接依赖MQTT或退出进程。
 */

#include <expected>
#include <functional>
#include <string_view>

#include <nlohmann/json.hpp>

#include "config_command/command_parser.hpp"

namespace config_command {

struct CommandProcessResult {
  nlohmann::json ack;
  bool should_exit = false;
};

using PersistFunction =
    std::function<std::expected<void, CommandError>(const nlohmann::json&)>;

CommandProcessResult ProcessConfigCommand(std::string_view payload,
                                          const nlohmann::json& current,
                                          const PersistFunction& persist);

}  // namespace config_command
