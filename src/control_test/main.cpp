#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

#include "common/mavlink.h"
#include "config/app_config.hpp"
#include "control_command/control_command.hpp"
#include "control_command/control_endpoint.hpp"
#include "control_command/control_transaction.hpp"
#include "control_test/control_test_cli.hpp"
#include "uart/mavlink_link.hpp"

namespace {

constexpr std::uint8_t kControlSystemId = 250;
constexpr std::uint8_t kControlComponentId = MAV_COMP_ID_ONBOARD_COMPUTER;
constexpr auto kHeartbeatWaitTimeout = std::chrono::seconds(5);
constexpr auto kCommandAckTimeout = std::chrono::seconds(2);
constexpr int kCommandFailed = 1;
constexpr int kUsageOrConfigError = 2;
constexpr int kUartError = 3;

void PrintAck(const nlohmann::json& ack) {
  std::cout << ack.dump() << '\n';
}

int Run(int argc, char** argv) {
  std::vector<std::string_view> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  const auto options = control_test::ParseArguments(arguments);
  if (!options) {
    std::cerr << options.error().message << '\n';
    return kUsageOrConfigError;
  }

  const auto payload = control_test::LoadPayload(*options);
  if (!payload) {
    std::cerr << payload.error().message << '\n';
    return kUsageOrConfigError;
  }

  const auto command = control_command::Parse(*payload);
  if (!command) {
    PrintAck(control_command::BuildRejectedAck(
        command.error().command_id, command.error().command, command.error()));
    return kCommandFailed;
  }

  // 空运行必须在读取配置和打开串口之前结束，确保默认操作不接触硬件。
  if (!options->send) {
    PrintAck(control_test::BuildDryRunResult(*command));
    return EXIT_SUCCESS;
  }

  const auto app_config = config::LoadAppConfig(options->config_path);
  if (!app_config) {
    std::cerr << "读取配置失败：" << options->config_path << "："
              << config::ConfigErrorMessage(app_config.error()) << '\n';
    return kUsageOrConfigError;
  }

  auto link = uart::MavlinkLink::Open(app_config->serial.device,
                                      app_config->serial.baud);
  if (!link) {
    std::cerr << "打开串口失败：" << app_config->serial.device << '\n';
    return kUartError;
  }

  std::optional<control_command::MavlinkEndpoint> endpoint;
  const auto heartbeat_deadline =
      std::chrono::steady_clock::now() + kHeartbeatWaitTimeout;
  while (!endpoint && std::chrono::steady_clock::now() < heartbeat_deadline) {
    if (const auto message = link->ReceiveMessage()) {
      endpoint = control_command::ObserveFlightControllerHeartbeat(*message, endpoint);
    }
  }
  if (!endpoint) {
    std::cerr << "等待 STM32 HEARTBEAT 超时，未发送控制命令\n";
    return kUartError;
  }

  std::cerr << "已发现 STM32 端点：sysid="
            << static_cast<unsigned int>(endpoint->system_id)
            << "，compid=" << static_cast<unsigned int>(endpoint->component_id)
            << "，准备下发命令：" << command->command << "，参数="
            << nlohmann::json(command->params).dump() << '\n';

  control_command::ControlTransaction transaction(kCommandAckTimeout, 1);
  const auto submitted = transaction.Submit(
      *command, control_command::ControlTransaction::Clock::now());
  if (!submitted.should_send_to_mcu) {
    if (submitted.ack) {
      PrintAck(*submitted.ack);
      return control_test::ExitCodeForFinalAck(*submitted.ack);
    }
    std::cerr << "控制事务未进入可发送状态\n";
    return kCommandFailed;
  }

  const auto message = control_command::EncodeCommandLong(
      *command, kControlSystemId, kControlComponentId, endpoint->system_id,
      endpoint->component_id);
  if (!link->SendMessage(message)) {
    (void)transaction.HandleLocalFailure(
        {.code = "uart_send_failed", .message = "控制命令发送到单片机失败"});
  }

  while (transaction.HasPending()) {
    const auto now = control_command::ControlTransaction::Clock::now();
    if (const auto received = link->ReceiveMessage();
        received && control_command::IsExpectedCommandAck(
                        *received, *endpoint, kControlSystemId,
                        kControlComponentId)) {
      mavlink_command_ack_t ack{};
      mavlink_msg_command_ack_decode(&*received, &ack);
      (void)transaction.HandleMavlinkAck(
          ack.command, ack.result, ack.progress, ack.result_param2,
          control_command::ControlTransaction::Clock::now());
    }

    (void)transaction.CheckTimeout(now);
    if (const auto* ack = transaction.PendingAck(); ack != nullptr) {
      const auto output = *ack;
      const bool is_final = transaction.PendingAckIsFinal();
      PrintAck(output);
      transaction.ConfirmAckPublished();
      if (is_final) {
        return control_test::ExitCodeForFinalAck(output);
      }
    }
  }

  std::cerr << "控制事务异常结束，未获得最终回执\n";
  return kCommandFailed;
}

}  // namespace

int main(int argc, char** argv) { return Run(argc, argv); }
