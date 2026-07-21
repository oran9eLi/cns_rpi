#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "common/mavlink.h"

namespace control_command {

constexpr std::uint16_t kSetMotorPwmUs = 31013;
constexpr std::uint16_t kEmergencyStop = 31090;
constexpr std::uint16_t kAutoTakeoff = 31091;
constexpr std::uint16_t kAutoLanding = 31092;

struct ControlCommand {
  std::string command_id;
  std::string command;
  std::uint16_t mavlink_command = 0;
  std::array<float, 4> params{};
};

struct CommandError {
  std::string code;
  std::string message;
  // JSON字段类型可用时保留，便于拒绝回执关联原请求。
  std::string command_id{};
  std::string command{};
};

std::expected<ControlCommand, CommandError> Parse(std::string_view payload);
mavlink_message_t EncodeCommandLong(const ControlCommand& command, std::uint8_t source_system,
                                    std::uint8_t source_component,
                                    std::uint8_t target_system,
                                    std::uint8_t target_component);
nlohmann::json BuildRejectedAck(std::string_view command_id, std::string_view command,
                                const CommandError& error);
nlohmann::json BuildMavlinkAck(const ControlCommand& command, std::uint8_t result,
                               std::uint8_t progress = 0,
                               std::int32_t result_param2 = 0);
nlohmann::json BuildTimeoutAck(const ControlCommand& command);

/// 把 MAV_RESULT 数值转成回执里用的可读结果码，也供日志复用。
std::string ResultCode(std::uint8_t result);

}  // namespace control_command
