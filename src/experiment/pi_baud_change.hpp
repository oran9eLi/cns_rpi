#pragma once

/** @file pi_baud_change.hpp @brief Pi 运行中持久化并读回串口速率。 */

#include <expected>
#include <filesystem>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "config_command/config_store.hpp"

namespace experiment {

struct PiBaudApplied { int applied_baud{}; };
struct PiBaudFailure {
  std::string code;
  std::string message;
  bool configuration_persisted{};
};

using BaudPersist = std::function<std::expected<void, config_command::CommandError>(
    const nlohmann::json&)>;
using BaudClose = std::function<void()>;
using BaudOpen = std::function<std::expected<int, std::string>(int)>;

/** @brief 先原子写配置，再由主循环唯一串口所有者关闭、重开并读回。 */
std::expected<PiBaudApplied, PiBaudFailure> ApplyPiBaud(
    const std::filesystem::path& config_path, int target_baud,
    const BaudPersist& persist, const BaudClose& close,
    const BaudOpen& open);

}  // namespace experiment
