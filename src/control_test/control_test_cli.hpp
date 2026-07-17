#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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

}  // namespace control_test
