/**
 * @file command_processor.cpp
 * @brief command_processor.hpp 的实现。
 */

#include "config_command/command_processor.hpp"

#include "config_command/config_updater.hpp"

namespace config_command {
namespace {
std::string ExtractCommandId(std::string_view payload) {
  try {
    const auto root = nlohmann::json::parse(payload);
    if (root.is_object() && root.contains("command_id") && root["command_id"].is_string()) {
      return root["command_id"].get<std::string>();
    }
  } catch (const nlohmann::json::exception&) {
  }
  return {};
}
}  // namespace

CommandProcessResult ProcessConfigCommand(std::string_view payload,
                                          const nlohmann::json& current,
                                          const PersistFunction& persist) {
  auto command = ParseConfigCommand(payload);
  if (!command) {
    return {.ack = BuildRejectedAck(ExtractCommandId(payload), command.error()),
            .should_exit = false};
  }
  if (IsCommandApplied(current, command->command_id)) {
    return {.ack = {{"command_id", command->command_id},
                    {"status", "already_applied"},
                    {"restart_required", false}},
            .should_exit = false};
  }
  auto candidate = BuildUpdatedConfig(current, *command);
  if (!candidate) {
    return {.ack = BuildRejectedAck(command->command_id, candidate.error()),
            .should_exit = false};
  }
  auto persisted = persist(*candidate);
  if (!persisted) {
    return {.ack = BuildRejectedAck(command->command_id, persisted.error()),
            .should_exit = false};
  }
  return {.ack = {{"command_id", command->command_id},
                  {"status", "applied"},
                  {"restart_required", true}},
          .should_exit = true};
}

}  // namespace config_command
