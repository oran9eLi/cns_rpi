#pragma once

#include <cstdint>
#include <optional>

#include "common/mavlink.h"
#include "device/device_type.hpp"

namespace control_command {

// 固件在 USART6 发送前会将帧头 component id 改写为 193。
constexpr std::uint8_t kStm32Usart6ComponentId = 193;

struct MavlinkEndpoint {
  std::uint8_t system_id = 0;
  std::uint8_t component_id = 0;
};

/**
 * @brief 当前串口链路上已确认的受控设备。
 */
struct ControlledDeviceEndpoint {
  MavlinkEndpoint endpoint;
  device::Type type = device::Type::kUnknown;
};

/**
 * @brief 独立判断单个 HEARTBEAT 的设备类型，不受当前锁定端点影响。
 */
std::optional<ControlledDeviceEndpoint> ClassifyControlledDeviceHeartbeat(
    const mavlink_message_t& message, device::DetectionMode mode);

/**
 * @brief 从心跳中识别主控箱或 PX4 飞控。
 *
 * 自动模式使用严格特征避免误识别：主控箱必须是 USART6 组件 193 的
 * ONBOARD_CONTROLLER，PX4 必须声明 MAV_AUTOPILOT_PX4。
 */
std::optional<ControlledDeviceEndpoint> ObserveControlledDeviceHeartbeat(
    const mavlink_message_t& message,
    std::optional<ControlledDeviceEndpoint> current_endpoint,
    device::DetectionMode mode);

/**
 * @brief 判断消息是否属于已确认设备。
 *
 * 遥测与 Remote ID 允许来自同一 MAVLink system 的其他 component；
 * 心跳、AUTOPILOT_VERSION 和命令应答必须来自已确认 component。
 */
bool IsMessageFromControlledDevice(
    const mavlink_message_t& message,
    const ControlledDeviceEndpoint& controlled_device);

// 主控箱控制测试程序保留的兼容入口。
std::optional<MavlinkEndpoint> ObserveFlightControllerHeartbeat(
    const mavlink_message_t& message,
    std::optional<MavlinkEndpoint> current_endpoint);

std::optional<std::uint8_t> LearnedControlledSystemId(
    const std::optional<ControlledDeviceEndpoint>& controlled_device);

std::optional<std::uint8_t> LearnedSystemId(
    const std::optional<MavlinkEndpoint>& stm32_endpoint);

bool IsExpectedCommandAck(const mavlink_message_t& message,
                          const MavlinkEndpoint& stm32_endpoint,
                          std::uint8_t rpi_system_id,
                          std::uint8_t rpi_component_id);

}  // namespace control_command
