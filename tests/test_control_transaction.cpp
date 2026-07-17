#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>

#include "control_command/control_transaction.hpp"

namespace {

using namespace std::chrono_literals;
using control_command::ControlCommand;
using control_command::ControlTransaction;
using control_command::SubmitStatus;

ControlCommand Takeoff(std::string command_id) {
  return {.command_id = std::move(command_id),
          .command = "takeoff",
          .mavlink_command = control_command::kAutoTakeoff};
}

ControlCommand Landing(std::string command_id) {
  return {.command_id = std::move(command_id),
          .command = "land",
          .mavlink_command = control_command::kAutoLanding};
}

}  // namespace

TEST_CASE("首次命令进入等待状态且允许下发单片机") {
  ControlTransaction transaction(2s);

  const auto result = transaction.Submit(Takeoff("cmd-1"), ControlTransaction::TimePoint{});

  CHECK(result.status == SubmitStatus::kAccepted);
  CHECK(result.should_send_to_mcu);
  CHECK(transaction.HasPending());
}

TEST_CASE("执行中的重复command_id不会重复下发") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);

  const auto duplicate = transaction.Submit(Takeoff("cmd-1"), now + 10ms);

  CHECK(duplicate.status == SubmitStatus::kDuplicatePending);
  CHECK_FALSE(duplicate.should_send_to_mcu);
  REQUIRE(duplicate.ack.has_value());
  CHECK((*duplicate.ack)["status"] == "in_progress");
  CHECK((*duplicate.ack)["result_code"] == "pending");
}

TEST_CASE("相同command_id对应不同命令时拒绝冲突") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);

  const auto conflict = transaction.Submit(Landing("cmd-1"), now + 10ms);

  CHECK(conflict.status == SubmitStatus::kCommandIdConflict);
  CHECK_FALSE(conflict.should_send_to_mcu);
  REQUIRE(conflict.ack.has_value());
  CHECK((*conflict.ack)["error_code"] == "command_id_conflict");
}

TEST_CASE("单pending期间拒绝不同的新命令") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);

  const auto busy = transaction.Submit(Landing("cmd-2"), now + 10ms);

  CHECK(busy.status == SubmitStatus::kBusy);
  CHECK_FALSE(busy.should_send_to_mcu);
  REQUIRE(busy.ack.has_value());
  CHECK((*busy.ack)["error_code"] == "command_busy");
}

TEST_CASE("IN_PROGRESS保留事务并生成进度回执") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);

  const auto result = transaction.HandleMavlinkAck(
      control_command::kAutoTakeoff, MAV_RESULT_IN_PROGRESS, 35, 7, now + 1s);

  CHECK(result == control_command::MavlinkAckStatus::kInProgress);
  CHECK(transaction.HasPending());
  REQUIRE(transaction.PendingAck() != nullptr);
  CHECK((*transaction.PendingAck())["status"] == "in_progress");
  CHECK((*transaction.PendingAck())["progress"] == 35);
  CHECK((*transaction.PendingAck())["result_param2"] == 7);
}

TEST_CASE("IN_PROGRESS发布后仍能接收最终ACK并延长超时") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                       MAV_RESULT_IN_PROGRESS, 35, 7, now + 1500ms) ==
          control_command::MavlinkAckStatus::kInProgress);

  transaction.ConfirmAckPublished();

  CHECK(transaction.PendingAck() == nullptr);
  CHECK_FALSE(transaction.CheckTimeout(now + 2500ms));
  CHECK(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                     MAV_RESULT_ACCEPTED, 100, 0, now + 2600ms) ==
        control_command::MavlinkAckStatus::kFinal);
  REQUIRE(transaction.PendingAck() != nullptr);
  CHECK((*transaction.PendingAck())["status"] == "accepted");
}

TEST_CASE("终态ACK等待发布成功后才缓存完成结果") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                       MAV_RESULT_ACCEPTED, 0, 0, now + 1s) ==
          control_command::MavlinkAckStatus::kFinal);
  REQUIRE(transaction.PendingAck() != nullptr);
  CHECK(transaction.HasPending());

  transaction.ConfirmAckPublished();

  CHECK_FALSE(transaction.HasPending());
  CHECK(transaction.PendingAck() == nullptr);
  const auto replay = transaction.Submit(Takeoff("cmd-1"), now + 1500ms);
  CHECK(replay.status == SubmitStatus::kReplayCompleted);
  CHECK_FALSE(replay.should_send_to_mcu);
  REQUIRE(replay.ack.has_value());
  CHECK((*replay.ack)["status"] == "accepted");
}

TEST_CASE("完成后相同command_id但内容不同仍返回冲突") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                       MAV_RESULT_ACCEPTED, 0, 0, now + 1s) ==
          control_command::MavlinkAckStatus::kFinal);
  transaction.ConfirmAckPublished();

  const auto conflict = transaction.Submit(Landing("cmd-1"), now + 1500ms);

  CHECK(conflict.status == SubmitStatus::kCommandIdConflict);
  CHECK_FALSE(conflict.should_send_to_mcu);
}

TEST_CASE("超时产生最终回执并等待发布确认") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);

  CHECK_FALSE(transaction.CheckTimeout(now + 1999ms));
  CHECK(transaction.CheckTimeout(now + 2s));
  CHECK(transaction.HasPending());
  REQUIRE(transaction.PendingAck() != nullptr);
  CHECK(transaction.PendingAckIsFinal());
  CHECK((*transaction.PendingAck())["status"] == "timeout");

  transaction.ConfirmAckPublished();
  const auto replay = transaction.Submit(Takeoff("cmd-1"), now + 3s);
  REQUIRE(replay.ack.has_value());
  CHECK((*replay.ack)["status"] == "timeout");
}

TEST_CASE("IN_PROGRESS回执不是终态") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-progress"), now).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                       MAV_RESULT_IN_PROGRESS, 10, 0, now + 1s) ==
          control_command::MavlinkAckStatus::kInProgress);
  CHECK_FALSE(transaction.PendingAckIsFinal());
}

TEST_CASE("UART发送失败生成最终回执且发布后可重放") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);

  CHECK(transaction.HandleLocalFailure(
      {.code = "uart_send_failed", .message = "控制命令写入串口失败"}));

  CHECK(transaction.HasPending());
  REQUIRE(transaction.PendingAck() != nullptr);
  CHECK((*transaction.PendingAck())["status"] == "rejected");
  CHECK((*transaction.PendingAck())["error_code"] == "uart_send_failed");

  transaction.ConfirmAckPublished();
  const auto replay = transaction.Submit(Takeoff("cmd-1"), now + 1s);
  CHECK(replay.status == SubmitStatus::kReplayCompleted);
  REQUIRE(replay.ack.has_value());
  CHECK((*replay.ack)["error_code"] == "uart_send_failed");
}

TEST_CASE("无pending或已有终态时忽略本地失败") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  CHECK_FALSE(transaction.HandleLocalFailure(
      {.code = "uart_send_failed", .message = "控制命令写入串口失败"}));

  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                       MAV_RESULT_ACCEPTED, 0, 0, now + 1s) ==
          control_command::MavlinkAckStatus::kFinal);
  CHECK_FALSE(transaction.HandleLocalFailure(
      {.code = "uart_send_failed", .message = "控制命令写入串口失败"}));
  CHECK((*transaction.PendingAck())["status"] == "accepted");
}

TEST_CASE("不匹配当前命令的ACK会被忽略") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);

  CHECK(transaction.HandleMavlinkAck(control_command::kAutoLanding,
                                     MAV_RESULT_ACCEPTED, 0, 0, now + 1s) ==
        control_command::MavlinkAckStatus::kIgnored);
  CHECK(transaction.PendingAck() == nullptr);
  CHECK(transaction.HasPending());
}

TEST_CASE("超时后短期内拒绝同类新命令以隔离迟到ACK") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-old"), now).should_send_to_mcu);
  REQUIRE(transaction.CheckTimeout(now + 2s));
  transaction.ConfirmAckPublished();

  const auto guarded = transaction.Submit(Takeoff("cmd-new"), now + 3s);
  CHECK_FALSE(guarded.should_send_to_mcu);
  REQUIRE(guarded.ack.has_value());
  CHECK((*guarded.ack)["error_code"] == "stale_ack_guard");

  const auto accepted = transaction.Submit(Takeoff("cmd-later"), now + 12s);
  CHECK(accepted.should_send_to_mcu);
}

TEST_CASE("不同命令的超时隔离窗口不会互相覆盖") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("takeoff-old"), now).should_send_to_mcu);
  REQUIRE(transaction.CheckTimeout(now + 2s));
  transaction.ConfirmAckPublished();
  REQUIRE(transaction.Submit(Landing("land-old"), now + 3s).should_send_to_mcu);
  REQUIRE(transaction.CheckTimeout(now + 5s));
  transaction.ConfirmAckPublished();

  const auto takeoff = transaction.Submit(Takeoff("takeoff-new"), now + 6s);
  CHECK_FALSE(takeoff.should_send_to_mcu);
  REQUIRE(takeoff.ack.has_value());
  CHECK((*takeoff.ack)["error_code"] == "stale_ack_guard");
}

TEST_CASE("身份作用域重置后不重放旧设备命令") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("same-id"), now).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                       MAV_RESULT_ACCEPTED, 0, 0, now + 1s) ==
          control_command::MavlinkAckStatus::kFinal);
  transaction.ConfirmAckPublished();

  transaction.ResetIdentityScope();
  const auto new_identity = transaction.Submit(Takeoff("same-id"), now + 2s);
  CHECK(new_identity.should_send_to_mcu);
}

TEST_CASE("完成缓存按容量淘汰最早结果") {
  ControlTransaction transaction(2s, 1);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                       MAV_RESULT_ACCEPTED, 0, 0, now + 100ms) ==
          control_command::MavlinkAckStatus::kFinal);
  transaction.ConfirmAckPublished();
  REQUIRE(transaction.Submit(Landing("cmd-2"), now + 200ms).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kAutoLanding,
                                       MAV_RESULT_ACCEPTED, 0, 0, now + 300ms) ==
          control_command::MavlinkAckStatus::kFinal);
  transaction.ConfirmAckPublished();

  const auto evicted = transaction.Submit(Takeoff("cmd-1"), now + 400ms);

  CHECK(evicted.status == SubmitStatus::kAccepted);
  CHECK(evicted.should_send_to_mcu);
}
