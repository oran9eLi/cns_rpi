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
          {"error_code", "stale_ack_guard"},
          {"message", "同类命令刚刚超时，暂停下发以隔离迟到应答"}};
}

}  // namespace

ControlTransaction::ControlTransaction(std::chrono::milliseconds timeout,
                                       std::size_t completed_capacity)
    : timeout_(timeout), completed_capacity_(completed_capacity) {}

SubmitResult ControlTransaction::Submit(const ControlCommand& command, TimePoint now) {
  if (pending_ && pending_->command.command_id == command.command_id) {
    if (!SameContent(pending_->command, command)) {
      return {.status = SubmitStatus::kCommandIdConflict,
              .should_send_to_mcu = false,
              .ack = BuildConflictAck(command)};
    }
    return {.status = SubmitStatus::kDuplicatePending,
            .should_send_to_mcu = false,
            .ack = pending_ack_ ? pending_ack_
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

  if (pending_) {
    return {.status = SubmitStatus::kBusy,
            .should_send_to_mcu = false,
            .ack = BuildBusyAck(command)};
  }

  pending_ = PendingCommand{.command = command, .deadline = now + timeout_};
  return {.status = SubmitStatus::kAccepted,
          .should_send_to_mcu = true,
          .ack = std::nullopt};
}

MavlinkAckStatus ControlTransaction::HandleMavlinkAck(std::uint16_t mavlink_command,
                                                     std::uint8_t result,
                                                     std::uint8_t progress,
                                                     std::int32_t result_param2,
                                                     TimePoint now) {
  if (!pending_ || pending_->command.mavlink_command != mavlink_command ||
      pending_ack_is_final_) {
    return MavlinkAckStatus::kIgnored;
  }

  pending_ack_ = BuildMavlinkAck(pending_->command, result, progress, result_param2);
  pending_ack_is_final_ = result != MAV_RESULT_IN_PROGRESS;
  if (!pending_ack_is_final_) {
    pending_->deadline = now + timeout_;
    return MavlinkAckStatus::kInProgress;
  }
  return MavlinkAckStatus::kFinal;
}

bool ControlTransaction::HandleLocalFailure(const CommandError& error) {
  if (!pending_ || pending_ack_is_final_) {
    return false;
  }
  pending_ack_ =
      BuildRejectedAck(pending_->command.command_id, pending_->command.command, error);
  pending_ack_is_final_ = true;
  return true;
}

bool ControlTransaction::CheckTimeout(TimePoint now) {
  if (!pending_ || pending_ack_is_final_ || now < pending_->deadline) {
    return false;
  }
  pending_ack_ = BuildTimeoutAck(pending_->command);
  pending_ack_is_final_ = true;
  stale_ack_guards_[pending_->command.mavlink_command] = now + timeout_ * 5;
  return true;
}

bool ControlTransaction::HasPending() const { return pending_.has_value(); }

const nlohmann::json* ControlTransaction::PendingAck() const {
  return pending_ack_ ? &*pending_ack_ : nullptr;
}

bool ControlTransaction::PendingAckIsFinal() const {
  return pending_ack_ && pending_ack_is_final_;
}

void ControlTransaction::ResetIdentityScope() {
  if (pending_) {
    return;
  }
  completed_.clear();
  stale_ack_guards_.clear();
}

void ControlTransaction::ConfirmAckPublished() {
  if (!pending_ack_) {
    return;
  }
  if (!pending_ack_is_final_) {
    pending_ack_.reset();
    return;
  }

  if (pending_) {
    CacheCompleted(pending_->command, *pending_ack_);
  }
  pending_.reset();
  pending_ack_.reset();
  pending_ack_is_final_ = false;
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
