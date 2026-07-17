#include "control_test/control_test_cli.hpp"

#include <fstream>
#include <iterator>

#include "control_command/control_endpoint.hpp"

namespace control_test {
namespace {

std::unexpected<CliError> Error(std::string code, std::string message) {
  return std::unexpected(CliError{std::move(code), std::move(message)});
}

bool IsOption(std::string_view argument) {
  return argument.starts_with("--");
}

}  // namespace

std::expected<CliOptions, CliError> ParseArguments(
    std::span<const std::string_view> arguments) {
  CliOptions options;
  bool saw_send = false;
  bool saw_file = false;

  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument == "--send") {
      if (saw_send) {
        return Error("duplicate_option", "选项 --send 不能重复指定");
      }
      saw_send = true;
      options.send = true;
      continue;
    }

    if (argument == "--file") {
      if (saw_file) {
        return Error("duplicate_option", "选项 --file 不能重复指定");
      }
      if (index + 1 >= arguments.size() || IsOption(arguments[index + 1])) {
        return Error("missing_option_value", "选项 --file 后必须提供命令文件路径");
      }
      saw_file = true;
      options.payload_file = std::filesystem::path(arguments[++index]);
      continue;
    }

    if (IsOption(argument)) {
      return Error("unknown_option", "未知选项：" + std::string(argument));
    }

    if (options.config_path.empty()) {
      options.config_path = std::filesystem::path(argument);
    } else if (!options.inline_payload.has_value()) {
      options.inline_payload = std::string(argument);
    } else {
      return Error("too_many_arguments", "普通参数过多，只允许配置路径和一段直接命令内容");
    }
  }

  if (options.config_path.empty()) {
    return Error("missing_config_path", "缺少配置文件路径");
  }
  if (options.inline_payload.has_value() && options.payload_file.has_value()) {
    return Error("conflicting_payload_sources", "直接命令内容与 --file 不能同时使用");
  }
  if (!options.inline_payload.has_value() && !options.payload_file.has_value()) {
    return Error("missing_payload", "缺少控制命令内容");
  }

  return options;
}

std::expected<std::string, CliError> LoadPayload(const CliOptions& options) {
  if (options.inline_payload.has_value()) {
    return *options.inline_payload;
  }
  if (!options.payload_file.has_value()) {
    return Error("missing_payload", "缺少控制命令内容");
  }

  std::ifstream input(*options.payload_file, std::ios::binary);
  if (!input.is_open()) {
    return Error("payload_file_unreadable",
                 "无法读取命令文件：" + options.payload_file->string());
  }

  std::string payload{std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>()};
  if (payload.empty()) {
    return Error("empty_payload", "命令文件为空");
  }
  return payload;
}

nlohmann::json BuildDryRunResult(
    const control_command::ControlCommand& command) {
  const auto message = control_command::EncodeCommandLong(
      command, 250, MAV_COMP_ID_ONBOARD_COMPUTER, 0,
      control_command::kStm32Usart6ComponentId);
  mavlink_command_long_t packet{};
  mavlink_msg_command_long_decode(&message, &packet);

  return {{"status", "dry_run"},
          {"command_id", command.command_id},
          {"mavlink_command", packet.command},
          {"target_system", nullptr},
          {"target_component", packet.target_component},
          {"params", nlohmann::json::array(
                         {packet.param1, packet.param2, packet.param3,
                          packet.param4, packet.param5, packet.param6,
                          packet.param7})}};
}

int ExitCodeForFinalAck(const nlohmann::json& ack, bool uart_write_failed) {
  if (uart_write_failed) {
    return 3;
  }
  const auto status = ack.find("status");
  return status != ack.end() && status->is_string() && *status == "accepted" ? 0 : 1;
}

std::string UartOpenErrorMessage(uart::UartError error,
                                 std::string_view device) {
  if (error == uart::UartError::kDeviceBusy) {
    return "串口已被占用：" + std::string(device) +
           "；请先执行 systemctl stop cns-rpi.service 停止主服务";
  }
  return "打开串口失败：" + std::string(device);
}

}  // namespace control_test
