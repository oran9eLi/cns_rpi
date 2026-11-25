#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

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
 * @brief 管理主控箱命令从下发到受理回执发布的事务。
 *
 * 普通命令串行等待受理，不同命令号的上锁和急停可以优先下发。成功发布的
 * 受理结果保存在有界进程内缓存中，用于识别重复 command_id 并重放结果。
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
  bool HandleLocalFailure(const CommandError& error, TimePoint now = Clock::now());
  bool CheckTimeout(TimePoint now);

  [[nodiscard]] bool HasPending() const;
  [[nodiscard]] const nlohmann::json* PendingAck() const;
  [[nodiscard]] bool PendingAckIsFinal() const;

  /** @brief 控制事务跟随箱号，同一箱号重建 MQTT 会话时保留事务状态。 */
  void SetDeviceScope(std::string_view device_id);

  /** @brief 确认当前回执已经成功交给 MQTT 客户端。 */
  void ConfirmAckPublished();

 private:
  struct PendingCommand {
    ControlCommand command;
    TimePoint deadline;
    std::optional<nlohmann::json> ack;
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
  std::string device_id_;
  std::deque<PendingCommand> pending_;
  std::unordered_map<std::uint16_t, TimePoint> stale_ack_guards_;
  std::deque<CompletedCommand> completed_;
};

}  // namespace control_command
