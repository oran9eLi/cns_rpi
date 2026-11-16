#include "control_command/control_command.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace control_command {
namespace {

CommandError Error(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .command_id = {},
          .command = {}};
}

std::optional<float> IntegerInRange(const nlohmann::json& value,
                                   std::int64_t minimum, std::int64_t maximum) {
  if (!value.is_number_integer()) return std::nullopt;
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    if (number < static_cast<std::uint64_t>(minimum) ||
        number > static_cast<std::uint64_t>(maximum)) return std::nullopt;
    return static_cast<float>(number);
  }
  const auto number = value.get<std::int64_t>();
  if (number < minimum || number > maximum) return std::nullopt;
  return static_cast<float>(number);
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

  if (!root.contains("parameters") || !root["parameters"].is_object()) {
    return std::unexpected(contextual_error("invalid_parameter", "parameters必须提供对象"));
  }
  const auto& parameters = root["parameters"];

  if (result.command == "set_motor_pwm") {
    if (parameters.size() != 2 || !parameters.contains("motor_id") ||
        !parameters.contains("pwm_us")) {
      return std::unexpected(
          contextual_error("invalid_parameter", "set_motor_pwm只接受motor_id和pwm_us参数"));
    }
    const auto motor_id = IntegerInRange(parameters["motor_id"], 1, 4);
    if (!motor_id) {
      return std::unexpected(contextual_error("invalid_parameter", "motor_id必须为1到4的整数"));
    }
    const auto pulse_us = IntegerInRange(parameters["pwm_us"], 1000, 2000);
    if (!pulse_us) {
      return std::unexpected(contextual_error(
          "invalid_parameter", "pwm_us必须为1000到2000的整数"));
    }
    result.params[0] = *motor_id;
    result.params[1] = *pulse_us;
    result.mavlink_command = kSetMotorPwmUs;
  } else {
    if (!parameters.empty()) {
      return std::unexpected(contextual_error("invalid_parameter", "该命令不接受参数"));
    }
    if (result.command == "arm") {
      result.mavlink_command = kArm;
    } else if (result.command == "disarm") {
      result.mavlink_command = kDisarm;
    } else if (result.command == "takeoff") {
      result.mavlink_command = kAutoTakeoff;
    } else if (result.command == "land") {
      result.mavlink_command = kAutoLanding;
    } else if (result.command == "emergency_stop") {
      result.mavlink_command = kEmergencyStop;
    } else {
      return std::unexpected(contextual_error("unsupported_command", "不支持的控制命令"));
    }
  }

  return result;
}

bool IsPriorityCommand(const ControlCommand& command) {
  return command.mavlink_command == kDisarm ||
         command.mavlink_command == kEmergencyStop;
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
          {"message", "命令已下发，等待F407受理"}};
}

nlohmann::json BuildMavlinkAck(const ControlCommand& command, std::uint8_t result,
                               std::uint8_t /*progress*/, std::int32_t result_param2) {
  const bool accepted = result == MAV_RESULT_ACCEPTED;
  auto payload = nlohmann::json{{"command_id", command.command_id},
          {"command", command.command},
          {"mavlink_command", command.mavlink_command},
          {"status", accepted ? "accepted" : "rejected"},
          {"result", result},
          {"result_code", ResultCode(result)},
          {"result_param2", result_param2},
          {"message", accepted ? "F407已接受命令" : "F407拒绝命令"}};
  if (!accepted) {
    payload["error_code"] = "mcu_rejected";
    if (result == MAV_RESULT_UNSUPPORTED) {
      payload["error_code"] = "unsupported_command";
      payload["message"] = "F407不支持该命令";
    }
  }
  constexpr std::array<std::pair<std::string_view, std::string_view>, 5> reasons{{
      {"device_locked", "设备未解锁"},
      {"device_busy", "设备正在执行其他动作"},
      {"invalid_parameter", "命令参数非法"},
      {"invalid_state", "当前动作状态不允许"},
      {"device_check_failed", "设备内部起飞检查未通过"}}};
  if (!accepted && result_param2 >= 1 &&
      result_param2 <= static_cast<std::int32_t>(reasons.size())) {
    const auto& [code, message] = reasons[result_param2 - 1];
    payload["error_code"] = code;
    payload["message"] = message;
  }
  return payload;
}

nlohmann::json BuildTimeoutAck(const ControlCommand& command) {
  return {{"command_id", command.command_id},
          {"command", command.command},
          {"mavlink_command", command.mavlink_command},
          {"status", "timeout"},
          {"error_code", "mcu_ack_timeout"},
          {"message", "未收到F407受理结果，执行状态未知"}};
}

}  // namespace control_command
