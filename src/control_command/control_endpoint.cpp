#include "control_command/control_endpoint.hpp"

namespace control_command {

namespace {

bool IsValidSystemId(std::uint8_t system_id) {
  return system_id != 0 && system_id <= 250;
}

bool IsCnsBoxHeartbeat(const mavlink_message_t& message,
                       const mavlink_heartbeat_t& heartbeat) {
  return message.compid == kStm32Usart6ComponentId &&
         heartbeat.type == MAV_TYPE_ONBOARD_CONTROLLER &&
         heartbeat.autopilot == MAV_AUTOPILOT_INVALID;
}

}  // namespace

std::optional<ControlledDeviceEndpoint> ClassifyControlledDeviceHeartbeat(
    const mavlink_message_t& message) {
  if (message.msgid != MAVLINK_MSG_ID_HEARTBEAT ||
      !IsValidSystemId(message.sysid)) {
    return std::nullopt;
  }

  mavlink_heartbeat_t heartbeat{};
  mavlink_msg_heartbeat_decode(&message, &heartbeat);

  if (IsCnsBoxHeartbeat(message, heartbeat)) {
    return ControlledDeviceEndpoint{
        .endpoint =
            MavlinkEndpoint{.system_id = message.sysid,
                            .component_id = message.compid},
        .type = device::Type::kCnsBox};
  }

  return std::nullopt;
}

std::optional<ControlledDeviceEndpoint> ObserveControlledDeviceHeartbeat(
    const mavlink_message_t& message,
    std::optional<ControlledDeviceEndpoint> current_endpoint) {
  if (current_endpoint) {
    return current_endpoint;
  }
  return ClassifyControlledDeviceHeartbeat(message);
}

bool IsMessageFromControlledDevice(
    const mavlink_message_t& message,
    const ControlledDeviceEndpoint& controlled_device) {
  if (message.sysid != controlled_device.endpoint.system_id) {
    return false;
  }

  switch (message.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT:
    case MAVLINK_MSG_ID_COMMAND_ACK:
      return message.compid == controlled_device.endpoint.component_id;
    default:
      return true;
  }
}

std::optional<MavlinkEndpoint> ObserveCnsBoxHeartbeat(
    const mavlink_message_t& message,
    std::optional<MavlinkEndpoint> current_endpoint) {
  if (current_endpoint) {
    return current_endpoint;
  }
  const auto observed = ClassifyControlledDeviceHeartbeat(message);
  if (!observed) {
    return std::nullopt;
  }
  return observed->endpoint;
}

std::optional<std::uint8_t> LearnedControlledSystemId(
    const std::optional<ControlledDeviceEndpoint>& controlled_device) {
  if (!controlled_device) {
    return std::nullopt;
  }
  return controlled_device->endpoint.system_id;
}

std::optional<std::uint8_t> LearnedSystemId(
    const std::optional<MavlinkEndpoint>& stm32_endpoint) {
  if (!stm32_endpoint) {
    return std::nullopt;
  }
  return stm32_endpoint->system_id;
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
