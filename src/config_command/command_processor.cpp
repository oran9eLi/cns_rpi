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

std::expected<nlohmann::json, CommandError> BuildSuccessAck(
    const ConfigCommand& command, const nlohmann::json& config, bool already_applied) {
  try {
    const bool rates_only = !command.parameters.topic_publish_intervals_ms.empty();
    nlohmann::json ack{{"command_id", command.command_id},
        {"status", already_applied ? "already_applied" : "applied"},
        {"restart_required", !already_applied && !rates_only}};
    if (rates_only) {
      if (ReportTopics(config) != command.report_topics) {
        return std::unexpected(CommandError{
            "config_read_failed", "磁盘Topic后缀与运行配置不一致，请先加载配置"});
      }
      ack["topic_publish_intervals_ms"] = ReportIntervals(config);
    }
    return ack;
  } catch (const nlohmann::json::exception&) {
    return std::unexpected(CommandError{"config_read_failed", "当前上报周期配置结构非法"});
  }
}
}  // namespace

nlohmann::json BuildRejectedAckForPayload(std::string_view payload, const CommandError& error) {
  return BuildRejectedAck(ExtractCommandId(payload), error);
}

CommandProcessResult ProcessConfigCommand(std::string_view payload,
                                          const nlohmann::json& current,
                                          const PersistFunction& persist) {
  ReportTopicNames topics;
  try {
    topics = ReportTopics(current);
  } catch (const nlohmann::json::exception&) {
    return {.ack = BuildRejectedAckForPayload(payload,
        {"config_read_failed", "当前Topic配置结构非法"}), .should_exit = false};
  }
  auto command = ParseConfigCommand(payload, topics);
  if (!command) {
    return {.ack = BuildRejectedAckForPayload(payload, command.error()),
            .should_exit = false};
  }
  return ProcessConfigCommand(*command, current, persist);
}

CommandProcessResult ProcessConfigCommand(const ConfigCommand& command,
                                          const nlohmann::json& current,
                                          const PersistFunction& persist) {
  const bool rates_only = !command.parameters.topic_publish_intervals_ms.empty();
  const bool already_applied = IsCommandApplied(current, command.command_id);
  auto candidate = already_applied ? std::expected<nlohmann::json, CommandError>{current}
                                   : BuildUpdatedConfig(current, command);
  if (!candidate) {
    return {.ack = BuildRejectedAck(command.command_id, candidate.error()),
            .should_exit = false};
  }
  auto ack = BuildSuccessAck(command, *candidate, already_applied);
  if (!ack) {
    return {.ack = BuildRejectedAck(command.command_id, ack.error()), .should_exit = false};
  }
  if (already_applied) return {.ack = std::move(*ack), .should_exit = false};
  auto persisted = persist(*candidate);
  if (!persisted) {
    auto ack = BuildRejectedAck(command.command_id, persisted.error());
    const bool uncertain = persisted.error().code == "config_write_uncertain";
    if (uncertain) ack["restart_required"] = true;
    return {.ack = std::move(ack), .should_exit = uncertain};
  }
  return {.ack = std::move(*ack), .should_exit = !rates_only};
}

}  // namespace config_command
