#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "common/mavlink.h"
#include "control_command/control_endpoint.hpp"

namespace protocol {

std::optional<std::string> FormatPx4Uid2(
    const mavlink_autopilot_version_t& version);
std::optional<std::string> FormatPx4Uid(
    const mavlink_autopilot_version_t& version);

/**
 * @brief 从 PX4 的 AUTOPILOT_VERSION 生成稳定设备 ID。
 *
 * 优先使用 18 字节 uid2；uid2 全零时回退到 64 位 uid。两个字段都无效时
 * 返回 nullopt，绝不使用 sysid 或树莓派序列号冒充飞控硬件身份。
 */
std::optional<std::string> FormatPx4DeviceId(
    const mavlink_autopilot_version_t& version);

/**
 * @brief 构造请求 AUTOPILOT_VERSION(消息 148) 的标准 MAVLink 命令。
 */
mavlink_message_t BuildAutopilotVersionRequest(
    std::uint8_t source_system_id,
    std::uint8_t source_component_id,
    const control_command::MavlinkEndpoint& target);

}  // namespace protocol
