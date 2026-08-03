/**
 * @file px4_identity.cpp
 * @brief px4_identity.hpp 的实现。
 */

#include "protocol/px4_identity.hpp"

#include <string>

namespace protocol {

std::optional<device::ProductInfo> ExtractPx4ProductInfo(
    const mavlink_autopilot_version_t& version) {
  if (version.vendor_id == 0 && version.product_id == 0) {
    return std::nullopt;
  }
  return device::ProductInfo{
      .manufacturer_code = std::to_string(version.vendor_id),
      .model_code = std::to_string(version.product_id),
  };
}

std::optional<device::VersionInfo> ExtractPx4VersionInfo(
    const mavlink_autopilot_version_t& version) {
  device::VersionInfo info;
  if (version.board_version != 0) {
    info.hardware = std::to_string(version.board_version);
  }
  if (version.flight_sw_version != 0) {
    info.firmware =
        std::to_string((version.flight_sw_version >> 24) & 0xFFU) + "." +
        std::to_string((version.flight_sw_version >> 16) & 0xFFU) + "." +
        std::to_string((version.flight_sw_version >> 8) & 0xFFU);
  }
  if (!info.hardware && !info.firmware) {
    return std::nullopt;
  }
  return info;
}

mavlink_message_t BuildMessageRequest(std::uint32_t message_id,
                                      std::uint8_t source_system_id,
                                      std::uint8_t source_component_id,
                                      std::uint8_t target_system_id,
                                      std::uint8_t target_component_id) {
  mavlink_message_t message{};
  mavlink_msg_command_long_pack(
      source_system_id, source_component_id, &message, target_system_id,
      target_component_id, MAV_CMD_REQUEST_MESSAGE, 0,
      static_cast<float>(message_id), 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F);
  return message;
}

}  // namespace protocol
