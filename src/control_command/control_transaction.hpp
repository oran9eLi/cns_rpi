#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <optional>

#include <nlohmann/json.hpp>

#include "control_command/control_command.hpp"

namespace control_command {

enum class SubmitStatus {
  kAccepted,
  kDuplicatePending,
  kReplayCompleted,
  kBusy,
  kCommandIdConflict,
};

struct SubmitResult {
  SubmitStatus status = SubmitStatus::kBusy;
  bool should_send_to_mcu = false;
  std::optional<nlohmann::json> ack;
};

enum class MavlinkAckStatus {
  kIgnored,
  kInProgress,
  kFinal,
};

/**
 * @brief 管理单条飞控命令从下发到回执发布的完整事务。
 *
 * 一个实例同时只允许一条命令等待单片机响应。成功发布的最终回执会保存在
 * 有界的进程内缓存中，用于识别重复 command_id 并重放结果。
 */
class ControlTransaction {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  explicit ControlTransaction(std::chrono::milliseconds timeout,
                              std::size_t completed_capacity = 128);

  SubmitResult Submit(const ControlCommand& command, TimePoint now);
  MavlinkAckStatus HandleMavlinkAck(std::uint16_t mavlink_command, std::uint8_t result,
                                    std::uint8_t progress, std::int32_t result_param2,
                                    TimePoint now);
  bool HandleLocalFailure(const CommandError& error);
  bool CheckTimeout(TimePoint now);

  [[nodiscard]] bool HasPending() const;
  [[nodiscard]] const nlohmann::json* PendingAck() const;
  [[nodiscard]] bool PendingAckIsFinal() const;

  /** @brief 设备身份切换后清空该身份作用域的幂等与迟到应答状态。 */
  void ResetIdentityScope();

  /** @brief 确认当前回执已经成功交给 MQTT 客户端。 */
  void ConfirmAckPublished();

 private:
  struct PendingCommand {
    ControlCommand command;
    TimePoint deadline;
  };

  struct CompletedCommand {
    ControlCommand command;
    nlohmann::json ack;
  };

  [[nodiscard]] static bool SameContent(const ControlCommand& left,
                                        const ControlCommand& right);
  [[nodiscard]] const CompletedCommand* FindCompleted(std::string_view command_id) const;
  void CacheCompleted(const ControlCommand& command, const nlohmann::json& ack);

  std::chrono::milliseconds timeout_;
  std::size_t completed_capacity_;
  std::optional<PendingCommand> pending_;
  std::optional<nlohmann::json> pending_ack_;
  bool pending_ack_is_final_ = false;
  std::unordered_map<std::uint16_t, TimePoint> stale_ack_guards_;
  std::deque<CompletedCommand> completed_;
};

}  // namespace control_command
