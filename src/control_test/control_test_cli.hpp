#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "control_command/control_command.hpp"
#include "uart/serial_port.hpp"

namespace control_test {

struct CliOptions {
  bool send = false;
  std::filesystem::path config_path;
  std::optional<std::string> inline_payload;
  std::optional<std::filesystem::path> payload_file;
};

struct CliError {
  std::string code;
  std::string message;
};

std::expected<CliOptions, CliError> ParseArguments(
    std::span<const std::string_view> arguments);
std::expected<std::string, CliError> LoadPayload(const CliOptions& options);
nlohmann::json BuildDryRunResult(
    const control_command::ControlCommand& command);

int ExitCodeForFinalAck(const nlohmann::json& ack,
                        bool uart_write_failed = false);

std::string UartOpenErrorMessage(uart::UartError error,
                                 std::string_view device);

}  // namespace control_test
