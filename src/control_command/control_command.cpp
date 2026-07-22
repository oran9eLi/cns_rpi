#include "control_command/control_command.hpp"

#include <cstdint>
#include <utility>

namespace control_command {
namespace {

CommandError Error(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .command_id = {},
          .command = {}};
}

}  // namespace

std::string ResultCode(std::uint8_t result) {
  switch (result) {
    case MAV_RESULT_ACCEPTED:
      return "accepted";
    case MAV_RESULT_TEMPORARILY_REJECTED:
      return "temporarily_rejected";
    case MAV_RESULT_DENIED:
      return "denied";
    case MAV_RESULT_UNSUPPORTED:
      return "unsupported";
    case MAV_RESULT_FAILED:
      return "failed";
    case MAV_RESULT_IN_PROGRESS:
      return "in_progress";
    case MAV_RESULT_CANCELLED:
      return "cancelled";
    default:
      return "unknown_result";
  }
}

std::expected<ControlCommand, CommandError> Parse(std::string_view payload) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(payload);
  } catch (const nlohmann::json::parse_error&) {
    return std::unexpected(Error("invalid_json", "命令不是合法JSON"));
  }

  const std::string command_id =
      root.is_object() && root.contains("command_id") && root["command_id"].is_string()
          ? root["command_id"].get<std::string>()
          : std::string{};
  const std::string command_name =
      root.is_object() && root.contains("command") && root["command"].is_string()
          ? root["command"].get<std::string>()
          : std::string{};
  const auto contextual_error = [&](std::string code, std::string message) {
    auto error = Error(std::move(code), std::move(message));
    error.command_id = command_id;
    error.command = command_name;
    return error;
  };

  if (!root.is_object() || !root.contains("command_id") ||
      !root["command_id"].is_string()) {
    return std::unexpected(contextual_error("invalid_command_id", "command_id缺失或类型非法"));
  }
  if (!root.contains("command") || !root["command"].is_string()) {
    return std::unexpected(contextual_error("invalid_command", "command缺失或类型非法"));
  }

  ControlCommand result;
  result.command_id = root["command_id"].get<std::string>();
  result.command = root["command"].get<std::string>();
  if (result.command_id.empty() || result.command_id.size() > 128) {
    return std::unexpected(contextual_error("invalid_command_id", "command_id长度非法"));
  }

  const nlohmann::json empty_parameters = nlohmann::json::object();
  const auto& parameters = root.contains("parameters") ? root["parameters"] : empty_parameters;
  if (!parameters.is_object()) {
    return std::unexpected(contextual_error("invalid_parameter", "parameters必须是对象"));
  }

  if (result.command == "set_motor_pwm") {
    if (parameters.size() != 1 || !parameters.contains("pwm_us")) {
      return std::unexpected(
          contextual_error("invalid_parameter", "set_motor_pwm只接受pwm_us参数"));
    }
    if (!parameters.contains("pwm_us") || !parameters["pwm_us"].is_array() ||
        parameters["pwm_us"].size() != 4) {
      return std::unexpected(contextual_error("invalid_parameter", "pwm_us必须包含4路脉宽"));
    }
    for (std::size_t i = 0; i < result.params.size(); ++i) {
      const auto& value = parameters["pwm_us"][i];
      if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return std::unexpected(contextual_error(
            "invalid_parameter", "PWM脉宽必须在1000到2000us之间"));
      }
      std::int64_t pulse_us = 0;
      if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value > 2000U) {
          return std::unexpected(contextual_error(
              "invalid_parameter", "PWM脉宽必须在1000到2000us之间"));
        }
        pulse_us = static_cast<std::int64_t>(unsigned_value);
      } else {
        pulse_us = value.get<std::int64_t>();
      }
      if (pulse_us < 1000 || pulse_us > 2000) {
        return std::unexpected(contextual_error(
            "invalid_parameter", "PWM脉宽必须在1000到2000us之间"));
      }
      result.params[i] = static_cast<float>(pulse_us);
    }
    result.mavlink_command = kSetMotorPwmUs;
  } else if (result.command == "takeoff") {
    if (!parameters.empty()) {
      return std::unexpected(contextual_error("invalid_parameter", "takeoff不接受参数"));
    }
    result.mavlink_command = kAutoTakeoff;
  } else if (result.command == "land") {
    if (!parameters.empty()) {
      return std::unexpected(contextual_error("invalid_parameter", "land不接受参数"));
    }
    result.mavlink_command = kAutoLanding;
  } else if (result.command == "emergency_stop") {
    if (!parameters.empty()) {
      return std::unexpected(
          contextual_error("invalid_parameter", "emergency_stop不接受参数"));
    }
    result.mavlink_command = kEmergencyStop;
  } else {
    return std::unexpected(contextual_error("unsupported_command", "不支持的控制命令"));
  }

  return result;
}

mavlink_message_t EncodeCommandLong(const ControlCommand& command, std::uint8_t source_system,
                                    std::uint8_t source_component,
                                    std::uint8_t target_system,
                                    std::uint8_t target_component) {
  mavlink_message_t message{};
  mavlink_msg_command_long_pack(source_system, source_component, &message,
                                target_system, target_component,
                                command.mavlink_command, /*confirmation=*/0,
                                command.params[0], command.params[1], command.params[2],
                                command.params[3], 0.0F, 0.0F, 0.0F);
  return message;
}

nlohmann::json BuildRejectedAck(std::string_view command_id, std::string_view command,
                                const CommandError& error) {
  return {{"command_id", command_id},
          {"command", command},
          {"status", "rejected"},
          {"error_code", error.code},
          {"message", error.message}};
}

nlohmann::json BuildPendingAck(const ControlCommand& command) {
  return {{"command_id", command.command_id},
          {"command", command.command},
          {"mavlink_command", command.mavlink_command},
          {"status", "in_progress"},
          {"result_code", "pending"},
          {"message", "命令已下发，正在等待单片机应答"}};
}

nlohmann::json BuildMavlinkAck(const ControlCommand& command, std::uint8_t result,
                               std::uint8_t progress, std::int32_t result_param2) {
  const bool accepted = result == MAV_RESULT_ACCEPTED;
  const bool in_progress = result == MAV_RESULT_IN_PROGRESS;
  auto payload = nlohmann::json{{"command_id", command.command_id},
          {"command", command.command},
          {"mavlink_command", command.mavlink_command},
          {"status", accepted ? "accepted" : (in_progress ? "in_progress" : "rejected")},
          {"result", result},
          {"result_code", ResultCode(result)},
          {"message", accepted ? "单片机已执行命令"
                               : (in_progress ? "单片机正在执行命令"
                                              : "单片机拒绝或执行命令失败")}};
  if (in_progress) {
    payload["progress"] = progress;
    payload["result_param2"] = result_param2;
  }
  return payload;
}

nlohmann::json BuildTimeoutAck(const ControlCommand& command) {
  return {{"command_id", command.command_id},
          {"command", command.command},
          {"mavlink_command", command.mavlink_command},
          {"status", "timeout"},
          {"error_code", "mcu_ack_timeout"},
          {"message", "等待单片机执行结果超时"}};
}

}  // namespace control_command
