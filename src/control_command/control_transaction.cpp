#include "control_command/control_transaction.hpp"

#include <algorithm>
#include "common/mavlink.h"

namespace control_command {
namespace {

nlohmann::json BuildConflictAck(const ControlCommand& command) {
  return {{"command_id", command.command_id},
          {"command", command.command},
          {"status", "rejected"},
          {"error_code", "command_id_conflict"},
          {"message", "command_id已用于其他命令"}};
}

nlohmann::json BuildBusyAck(const ControlCommand& command) {
  return {{"command_id", command.command_id},
          {"command", command.command},
          {"status", "rejected"},
          {"error_code", "command_busy"},
          {"message", "已有控制命令等待单片机响应"}};
}

nlohmann::json BuildStaleAckGuard(const ControlCommand& command) {
  return {{"command_id", command.command_id},
          {"command", command.command},
          {"status", "rejected"},
          {"error_code", "command_busy"},
          {"message", "同类命令的受理结果未知，暂停下发以隔离迟到应答"}};
}

}  // namespace

ControlTransaction::ControlTransaction(std::chrono::milliseconds timeout,
                                       std::size_t completed_capacity)
    : timeout_(timeout), completed_capacity_(completed_capacity) {}

SubmitResult ControlTransaction::Submit(const ControlCommand& command, TimePoint now) {
  const auto duplicate = std::find_if(
      pending_.begin(), pending_.end(), [&command](const PendingCommand& entry) {
        return entry.command.command_id == command.command_id;
      });
  if (duplicate != pending_.end()) {
    if (!SameContent(duplicate->command, command)) {
      return {.status = SubmitStatus::kCommandIdConflict,
              .should_send_to_mcu = false,
              .ack = BuildConflictAck(command)};
    }
    return {.status = SubmitStatus::kDuplicatePending,
            .should_send_to_mcu = false,
            .ack = duplicate->ack ? duplicate->ack
                                 : std::optional<nlohmann::json>(BuildPendingAck(command))};
  }

  if (const auto* completed = FindCompleted(command.command_id); completed != nullptr) {
    if (!SameContent(completed->command, command)) {
      return {.status = SubmitStatus::kCommandIdConflict,
              .should_send_to_mcu = false,
              .ack = BuildConflictAck(command)};
    }
    return {.status = SubmitStatus::kReplayCompleted,
            .should_send_to_mcu = false,
            .ack = std::optional<nlohmann::json>(completed->ack)};
  }

  const auto stale_guard = stale_ack_guards_.find(command.mavlink_command);
  if (stale_guard != stale_ack_guards_.end() && now < stale_guard->second) {
    return {.status = SubmitStatus::kBusy,
            .should_send_to_mcu = false,
            .ack = BuildStaleAckGuard(command)};
  }
  if (stale_guard != stale_ack_guards_.end()) {
    stale_ack_guards_.erase(stale_guard);
  }

  const bool same_command_pending = std::any_of(
      pending_.begin(), pending_.end(), [&command](const PendingCommand& entry) {
        return entry.command.mavlink_command == command.mavlink_command;
      });
  if (same_command_pending || (!pending_.empty() && !IsPriorityCommand(command))) {
    return {.status = SubmitStatus::kBusy,
            .should_send_to_mcu = false,
            .ack = BuildBusyAck(command)};
  }

  pending_.push_back({.command = command, .deadline = now + timeout_, .ack = std::nullopt});
  return {.status = SubmitStatus::kAccepted,
          .should_send_to_mcu = true,
          .ack = std::nullopt};
}

MavlinkAckStatus ControlTransaction::HandleMavlinkAck(std::uint16_t mavlink_command,
                                                     std::uint8_t result,
                                                     std::uint8_t progress,
                                                     std::int32_t result_param2,
                                                     TimePoint now) {
  CheckTimeout(now);
  if (result > MAV_RESULT_FAILED) {
    return MavlinkAckStatus::kIgnored;
  }
  const auto pending = std::find_if(
      pending_.begin(), pending_.end(), [mavlink_command](const PendingCommand& entry) {
        return entry.command.mavlink_command == mavlink_command && !entry.ack;
      });
  if (pending == pending_.end()) {
    return MavlinkAckStatus::kIgnored;
  }

  pending->ack = BuildMavlinkAck(pending->command, result, progress, result_param2);
  return MavlinkAckStatus::kFinal;
}

bool ControlTransaction::HandleLocalFailure(const CommandError& error, TimePoint now) {
  bool changed = false;
  for (auto& pending : pending_) {
    if (pending.ack) {
      continue;
    }
    pending.ack = BuildTimeoutAck(pending.command);
    (*pending.ack)["error_code"] = error.code;
    (*pending.ack)["message"] = error.message + "，未收到F407受理结果，执行状态未知";
    stale_ack_guards_[pending.command.mavlink_command] = now + timeout_ * 5;
    changed = true;
  }
  return changed;
}

bool ControlTransaction::CheckTimeout(TimePoint now) {
  bool changed = false;
  for (auto& pending : pending_) {
    if (pending.ack || now < pending.deadline) {
      continue;
    }
    pending.ack = BuildTimeoutAck(pending.command);
    stale_ack_guards_[pending.command.mavlink_command] = now + timeout_ * 5;
    changed = true;
  }
  return changed;
}

bool ControlTransaction::HasPending() const { return !pending_.empty(); }

const nlohmann::json* ControlTransaction::PendingAck() const {
  const auto ready = std::find_if(pending_.begin(), pending_.end(),
                                  [](const PendingCommand& entry) {
                                    return entry.ack.has_value();
                                  });
  return ready == pending_.end() ? nullptr : &*ready->ack;
}

bool ControlTransaction::PendingAckIsFinal() const {
  return PendingAck() != nullptr;
}

void ControlTransaction::SetDeviceScope(std::string_view device_id) {
  if (device_id_ == device_id) return;
  device_id_ = device_id;
  pending_.clear();
  completed_.clear();
  stale_ack_guards_.clear();
}

void ControlTransaction::ConfirmAckPublished() {
  const auto ready = std::find_if(pending_.begin(), pending_.end(),
                                  [](const PendingCommand& entry) {
                                    return entry.ack.has_value();
                                  });
  if (ready == pending_.end()) {
    return;
  }

  CacheCompleted(ready->command, *ready->ack);
  pending_.erase(ready);
}

bool ControlTransaction::SameContent(const ControlCommand& left,
                                     const ControlCommand& right) {
  return left.command == right.command && left.mavlink_command == right.mavlink_command &&
         left.params == right.params;
}

const ControlTransaction::CompletedCommand* ControlTransaction::FindCompleted(
    std::string_view command_id) const {
  const auto iterator = std::find_if(
      completed_.begin(), completed_.end(), [&command_id](const CompletedCommand& entry) {
        return entry.command.command_id == command_id;
      });
  return iterator == completed_.end() ? nullptr : &*iterator;
}

void ControlTransaction::CacheCompleted(const ControlCommand& command,
                                        const nlohmann::json& ack) {
  if (completed_capacity_ == 0) {
    return;
  }
  if (completed_.size() == completed_capacity_) {
    completed_.pop_front();
  }
  completed_.push_back({.command = command, .ack = ack});
}

}  // namespace control_command
