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

ControlCommand Disarm(std::string command_id) {
  return {.command_id = std::move(command_id),
          .command = "disarm",
          .mavlink_command = 31094};
}

ControlCommand EmergencyStop(std::string command_id) {
  return {.command_id = std::move(command_id),
          .command = "emergency_stop",
          .mavlink_command = control_command::kEmergencyStop};
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

TEST_CASE("非本版受理结果被忽略且不生成进度回执") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);

  const auto result = transaction.HandleMavlinkAck(
      control_command::kAutoTakeoff, MAV_RESULT_IN_PROGRESS, 35, 7, now + 1s);

  CHECK(result == control_command::MavlinkAckStatus::kIgnored);
  CHECK(transaction.HasPending());
  CHECK(transaction.PendingAck() == nullptr);
  CHECK(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                    MAV_RESULT_CANCELLED, 0, 0, now + 1100ms) ==
        control_command::MavlinkAckStatus::kIgnored);
  CHECK(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                    255, 0, 0, now + 1200ms) ==
        control_command::MavlinkAckStatus::kIgnored);
  CHECK(transaction.PendingAck() == nullptr);
}

TEST_CASE("IN_PROGRESS不延长两秒受理等待窗口") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                       MAV_RESULT_IN_PROGRESS, 35, 7, now + 1500ms) ==
          control_command::MavlinkAckStatus::kIgnored);

  CHECK(transaction.CheckTimeout(now + 2s));
  CHECK(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                     MAV_RESULT_ACCEPTED, 100, 0, now + 2600ms) ==
        control_command::MavlinkAckStatus::kIgnored);
  REQUIRE(transaction.PendingAck() != nullptr);
  CHECK((*transaction.PendingAck())["status"] == "timeout");
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

TEST_CASE("IN_PROGRESS不产生可发布回执") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-progress"), now).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                       MAV_RESULT_IN_PROGRESS, 10, 0, now + 1s) ==
          control_command::MavlinkAckStatus::kIgnored);
  CHECK_FALSE(transaction.PendingAckIsFinal());
  CHECK(transaction.PendingAck() == nullptr);
}

TEST_CASE("UART写入失败保留执行未知状态且发布后可重放") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("cmd-1"), now).should_send_to_mcu);

  CHECK(transaction.HandleLocalFailure(
      {.code = "uart_send_failed", .message = "控制命令写入串口失败"}));

  CHECK(transaction.HasPending());
  REQUIRE(transaction.PendingAck() != nullptr);
  CHECK((*transaction.PendingAck())["status"] == "timeout");
  CHECK((*transaction.PendingAck())["error_code"] == "uart_send_failed");
  CHECK(transaction.PendingAck()->at("message").get<std::string>().find("执行状态未知") !=
        std::string::npos);

  transaction.ConfirmAckPublished();
  const auto replay = transaction.Submit(Takeoff("cmd-1"), now + 1s);
  CHECK(replay.status == SubmitStatus::kReplayCompleted);
  REQUIRE(replay.ack.has_value());
  CHECK((*replay.ack)["status"] == "timeout");
  CHECK((*replay.ack)["error_code"] == "uart_send_failed");
}

TEST_CASE("链路断开后的未知结果继续隔离同号迟到ACK") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::Clock::now();
  REQUIRE(transaction.Submit(Takeoff("link-down-1"), now).should_send_to_mcu);

  REQUIRE(transaction.HandleLocalFailure(
      {.code = "stm32_link_unavailable", .message = "STM32串口链路不可用"}));
  REQUIRE(transaction.PendingAck() != nullptr);
  CHECK((*transaction.PendingAck())["status"] == "timeout");
  CHECK((*transaction.PendingAck())["error_code"] == "stm32_link_unavailable");
  transaction.ConfirmAckPublished();

  const auto blocked = transaction.Submit(Takeoff("link-down-2"), now + 1s);
  CHECK_FALSE(blocked.should_send_to_mcu);
  REQUIRE(blocked.ack.has_value());
  CHECK((*blocked.ack)["error_code"] == "command_busy");
  CHECK(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                    MAV_RESULT_ACCEPTED, 255, 0, now + 2s) ==
        control_command::MavlinkAckStatus::kIgnored);
  CHECK(transaction.Submit(Takeoff("link-restored"), now + 11s).should_send_to_mcu);
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
  CHECK((*guarded.ack)["error_code"] == "command_busy");

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
  CHECK((*takeoff.ack)["error_code"] == "command_busy");
}

TEST_CASE("同一箱号保留控制回执和隔离期，换箱后清理") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  const ControlCommand module{.command_id = "same-id", .command = "set_module_enabled",
                              .mavlink_command = control_command::kSetModuleEnabled,
                              .params = {4.0F, 1.0F}};
  ControlCommand pending = module;
  pending.command_id = "pending-id";
  transaction.SetDeviceScope("box-a");
  REQUIRE(transaction.Submit(module, now).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kSetModuleEnabled,
                                       MAV_RESULT_ACCEPTED, 0, 0, now + 1s) ==
          control_command::MavlinkAckStatus::kFinal);
  transaction.ConfirmAckPublished();

  REQUIRE(transaction.Submit(pending, now + 2s).should_send_to_mcu);
  REQUIRE(transaction.CheckTimeout(now + 4s));
  transaction.SetDeviceScope("box-a");
  REQUIRE(transaction.PendingAck() != nullptr);
  CHECK((*transaction.PendingAck())["command_id"] == "pending-id");
  CHECK(transaction.Submit(module, now + 4s).status == SubmitStatus::kReplayCompleted);

  transaction.SetDeviceScope("box-b");
  CHECK_FALSE(transaction.HasPending());
  CHECK(transaction.Submit(module, now + 5s).should_send_to_mcu);
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

TEST_CASE("普通受理等待不阻挡不同命令号的上锁和急停") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("takeoff"), now).should_send_to_mcu);
  REQUIRE(transaction.Submit(Disarm("disarm"), now + 10ms).should_send_to_mcu);
  REQUIRE(transaction.Submit(EmergencyStop("stop"), now + 20ms).should_send_to_mcu);

  CHECK(transaction.Submit(Landing("land"), now + 30ms).status == SubmitStatus::kBusy);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kEmergencyStop,
                                      MAV_RESULT_ACCEPTED, 255, 0, now + 50ms) ==
          control_command::MavlinkAckStatus::kFinal);
  REQUIRE(transaction.PendingAck() != nullptr);
  CHECK((*transaction.PendingAck())["command_id"] == "stop");
  transaction.ConfirmAckPublished();

  REQUIRE(transaction.HandleMavlinkAck(31094, MAV_RESULT_ACCEPTED, 255, 0,
                                      now + 60ms) ==
          control_command::MavlinkAckStatus::kFinal);
  CHECK((*transaction.PendingAck())["command_id"] == "disarm");
  transaction.ConfirmAckPublished();
  REQUIRE(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                      MAV_RESULT_DENIED, 255, 4, now + 70ms) ==
          control_command::MavlinkAckStatus::kFinal);
  CHECK((*transaction.PendingAck())["command_id"] == "takeoff");
  transaction.ConfirmAckPublished();
  CHECK_FALSE(transaction.HasPending());
}

TEST_CASE("优先命令先到回执不覆盖普通请求待发布结果") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("takeoff"), now).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                      MAV_RESULT_ACCEPTED, 255, 0, now + 10ms) ==
          control_command::MavlinkAckStatus::kFinal);
  REQUIRE(transaction.Submit(EmergencyStop("stop"), now + 20ms).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kEmergencyStop,
                                      MAV_RESULT_ACCEPTED, 255, 0, now + 30ms) ==
          control_command::MavlinkAckStatus::kFinal);
  CHECK((*transaction.PendingAck())["command_id"] == "takeoff");
  transaction.ConfirmAckPublished();
  CHECK((*transaction.PendingAck())["command_id"] == "stop");
  transaction.ConfirmAckPublished();
  CHECK_FALSE(transaction.HasPending());
  CHECK(transaction.Submit(Takeoff("takeoff"), now + 40ms).status ==
        SubmitStatus::kReplayCompleted);
  CHECK(transaction.Submit(EmergencyStop("stop"), now + 40ms).status ==
        SubmitStatus::kReplayCompleted);
}

TEST_CASE("同号优先请求只能重放同标识而不能并发另一标识") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(EmergencyStop("stop-1"), now).should_send_to_mcu);
  CHECK(transaction.Submit(EmergencyStop("stop-1"), now + 10ms).status ==
        SubmitStatus::kDuplicatePending);
  const auto busy = transaction.Submit(EmergencyStop("stop-2"), now + 20ms);
  CHECK_FALSE(busy.should_send_to_mcu);
  CHECK(busy.status == SubmitStatus::kBusy);
  CHECK((*busy.ack)["error_code"] == "command_busy");
  CHECK(transaction.Submit(Disarm("stop-1"), now + 30ms).status ==
        SubmitStatus::kCommandIdConflict);
}

TEST_CASE("仅优先命令等待时新的普通请求仍返回忙") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Disarm("disarm"), now).should_send_to_mcu);
  CHECK(transaction.Submit(Takeoff("takeoff"), now + 10ms).status == SubmitStatus::kBusy);
}

TEST_CASE("并行受理请求分别使用自己的两秒截止时间") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("takeoff"), now).should_send_to_mcu);
  REQUIRE(transaction.Submit(EmergencyStop("stop"), now + 1500ms).should_send_to_mcu);
  REQUIRE(transaction.CheckTimeout(now + 2s));
  CHECK((*transaction.PendingAck())["command_id"] == "takeoff");
  CHECK((*transaction.PendingAck())["status"] == "timeout");
  transaction.ConfirmAckPublished();
  CHECK(transaction.HasPending());
  CHECK(transaction.PendingAck() == nullptr);
  CHECK_FALSE(transaction.CheckTimeout(now + 3499ms));
  REQUIRE(transaction.CheckTimeout(now + 3500ms));
  CHECK((*transaction.PendingAck())["command_id"] == "stop");
  transaction.ConfirmAckPublished();
  CHECK_FALSE(transaction.HasPending());
}

TEST_CASE("一次超时检查可形成多个独立超时回执") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("takeoff"), now).should_send_to_mcu);
  REQUIRE(transaction.Submit(Disarm("disarm"), now + 10ms).should_send_to_mcu);
  REQUIRE(transaction.CheckTimeout(now + 3s));
  CHECK((*transaction.PendingAck())["command_id"] == "takeoff");
  transaction.ConfirmAckPublished();
  REQUIRE(transaction.PendingAck() != nullptr);
  CHECK((*transaction.PendingAck())["command_id"] == "disarm");
  CHECK((*transaction.PendingAck())["status"] == "timeout");
  transaction.ConfirmAckPublished();
  CHECK_FALSE(transaction.HasPending());
}

TEST_CASE("链路失败结束全部待受理项但不覆盖已形成的受理结果") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("takeoff"), now).should_send_to_mcu);
  REQUIRE(transaction.Submit(Disarm("disarm"), now + 10ms).should_send_to_mcu);
  REQUIRE(transaction.Submit(EmergencyStop("stop"), now + 20ms).should_send_to_mcu);
  REQUIRE(transaction.HandleMavlinkAck(31094, MAV_RESULT_ACCEPTED, 255, 0,
                                      now + 30ms) ==
          control_command::MavlinkAckStatus::kFinal);
  REQUIRE(transaction.HandleLocalFailure(
      {.code = "device_link_unavailable", .message = "链路不可用"}));
  CHECK_FALSE(transaction.HandleLocalFailure(
      {.code = "another_failure", .message = "不覆盖"}));
  CHECK((*transaction.PendingAck())["command_id"] == "takeoff");
  CHECK((*transaction.PendingAck())["status"] == "timeout");
  transaction.ConfirmAckPublished();
  CHECK((*transaction.PendingAck())["command_id"] == "disarm");
  CHECK((*transaction.PendingAck())["status"] == "accepted");
  transaction.ConfirmAckPublished();
  CHECK((*transaction.PendingAck())["command_id"] == "stop");
  CHECK((*transaction.PendingAck())["error_code"] == "device_link_unavailable");
  transaction.ConfirmAckPublished();
  CHECK_FALSE(transaction.HasPending());
}

TEST_CASE("普通命令超时不阻挡其他命令号的急停") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("takeoff"), now).should_send_to_mcu);
  REQUIRE(transaction.CheckTimeout(now + 2s));
  REQUIRE(transaction.Submit(EmergencyStop("stop"), now + 2100ms).should_send_to_mcu);
  transaction.ConfirmAckPublished();
  CHECK(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                    MAV_RESULT_ACCEPTED, 255, 0, now + 2200ms) ==
        control_command::MavlinkAckStatus::kIgnored);
  CHECK(transaction.PendingAck() == nullptr);
  REQUIRE(transaction.HandleMavlinkAck(control_command::kEmergencyStop,
                                      MAV_RESULT_ACCEPTED, 255, 0, now + 2300ms) ==
          control_command::MavlinkAckStatus::kFinal);
  CHECK((*transaction.PendingAck())["command_id"] == "stop");
}

TEST_CASE("ACK接收入口按两秒边界判断受理超时") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("takeoff"), now).should_send_to_mcu);

  SUBCASE("截止前一毫秒仍接受") {
    CHECK(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                      MAV_RESULT_ACCEPTED, 255, 0, now + 1999ms) ==
          control_command::MavlinkAckStatus::kFinal);
    REQUIRE(transaction.PendingAck() != nullptr);
    CHECK((*transaction.PendingAck())["status"] == "accepted");
  }

  SUBCASE("恰好到达截止时间时生成超时") {
    CHECK(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                      MAV_RESULT_ACCEPTED, 255, 0, now + 2s) ==
          control_command::MavlinkAckStatus::kIgnored);
    REQUIRE(transaction.PendingAck() != nullptr);
    CHECK((*transaction.PendingAck())["status"] == "timeout");
    transaction.ConfirmAckPublished();
    const auto guarded = transaction.Submit(Takeoff("takeoff-new"), now + 2001ms);
    CHECK_FALSE(guarded.should_send_to_mcu);
    CHECK(guarded.status == SubmitStatus::kBusy);
  }

  SUBCASE("未执行轮询检查也忽略迟到ACK") {
    CHECK(transaction.HandleMavlinkAck(control_command::kAutoTakeoff,
                                      MAV_RESULT_ACCEPTED, 255, 0, now + 2100ms) ==
          control_command::MavlinkAckStatus::kIgnored);
    REQUIRE(transaction.PendingAck() != nullptr);
    CHECK((*transaction.PendingAck())["status"] == "timeout");
  }
}

TEST_CASE("处理仍有效的优先ACK时先结算其他请求的超时") {
  ControlTransaction transaction(2s);
  const auto now = ControlTransaction::TimePoint{};
  REQUIRE(transaction.Submit(Takeoff("takeoff"), now).should_send_to_mcu);
  REQUIRE(transaction.Submit(EmergencyStop("stop"), now + 1500ms).should_send_to_mcu);

  REQUIRE(transaction.HandleMavlinkAck(control_command::kEmergencyStop,
                                      MAV_RESULT_ACCEPTED, 255, 0, now + 2100ms) ==
          control_command::MavlinkAckStatus::kFinal);

  REQUIRE(transaction.PendingAck() != nullptr);
  CHECK((*transaction.PendingAck())["command_id"] == "takeoff");
  CHECK((*transaction.PendingAck())["status"] == "timeout");
  transaction.ConfirmAckPublished();
  REQUIRE(transaction.PendingAck() != nullptr);
  CHECK((*transaction.PendingAck())["command_id"] == "stop");
  CHECK((*transaction.PendingAck())["status"] == "accepted");
  transaction.ConfirmAckPublished();
  CHECK_FALSE(transaction.HasPending());
}
