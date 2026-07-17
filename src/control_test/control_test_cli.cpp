#include "control_test/control_test_cli.hpp"

#include <fstream>
#include <iterator>

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

}  // namespace control_test
