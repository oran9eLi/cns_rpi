#include "control_command/control_endpoint.hpp"

namespace control_command {

std::optional<MavlinkEndpoint> ObserveFlightControllerHeartbeat(
    const mavlink_message_t& message,
    std::optional<MavlinkEndpoint> current_endpoint) {
  if (current_endpoint || message.msgid != MAVLINK_MSG_ID_HEARTBEAT) {
    return current_endpoint;
  }

  mavlink_heartbeat_t heartbeat{};
  mavlink_msg_heartbeat_decode(&message, &heartbeat);
  if (message.sysid == 0 || message.sysid > 250 ||
      message.compid != kStm32Usart6ComponentId ||
      heartbeat.type != MAV_TYPE_ONBOARD_CONTROLLER) {
    return std::nullopt;
  }
  return MavlinkEndpoint{.system_id = message.sysid,
                         .component_id = message.compid};
}

bool IsExpectedCommandAck(const mavlink_message_t& message,
                          const MavlinkEndpoint& stm32_endpoint,
                          std::uint8_t rpi_system_id,
                          std::uint8_t rpi_component_id) {
  if (message.msgid != MAVLINK_MSG_ID_COMMAND_ACK ||
      message.sysid != stm32_endpoint.system_id ||
      message.compid != stm32_endpoint.component_id) {
    return false;
  }

  mavlink_command_ack_t ack{};
  mavlink_msg_command_ack_decode(&message, &ack);
  return (ack.target_system == 0 || ack.target_system == rpi_system_id) &&
         (ack.target_component == 0 || ack.target_component == rpi_component_id);
}

}  // namespace control_command
