#pragma once

#include <cstdint>
#include <optional>

#include "common/mavlink.h"

namespace control_command {

// 固件在 USART6 发送前会将帧头 component id 改写为 193。
constexpr std::uint8_t kStm32Usart6ComponentId = 193;

struct MavlinkEndpoint {
  std::uint8_t system_id = 0;
  std::uint8_t component_id = 0;
};

std::optional<MavlinkEndpoint> ObserveFlightControllerHeartbeat(
    const mavlink_message_t& message,
    std::optional<MavlinkEndpoint> current_endpoint);

bool IsExpectedCommandAck(const mavlink_message_t& message,
                          const MavlinkEndpoint& stm32_endpoint,
                          std::uint8_t rpi_system_id,
                          std::uint8_t rpi_component_id);

}  // namespace control_command
