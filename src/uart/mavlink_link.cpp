/**
 * @file mavlink_link.cpp
 * @brief mavlink_link.hpp 的实现。
 */

#include "uart/mavlink_link.hpp"

#include <array>

namespace uart {

std::optional<mavlink_message_t> MavlinkFrameAssembler::Feed(std::span<const std::uint8_t> bytes) {
  for (std::uint8_t byte : bytes) {
    mavlink_message_t out_msg{};
    mavlink_status_t out_status{};
    std::uint8_t result = mavlink_frame_char_buffer(&rx_msg_, &status_, byte, &out_msg, &out_status);
    if (result == MAVLINK_FRAMING_OK) {
      pending_.push(out_msg);
    }
  }

  if (pending_.empty()) {
    return std::nullopt;
  }
  mavlink_message_t msg = pending_.front();
  pending_.pop();
  return msg;
}

std::expected<MavlinkLink, UartError> MavlinkLink::Open(const std::string& device, int baud) {
  auto port = SerialPort::Open(device, baud);
  if (!port) {
    return std::unexpected(port.error());
  }
  return MavlinkLink(std::move(*port));
}

std::optional<mavlink_message_t> MavlinkLink::ReceiveMessage() {
  std::array<std::uint8_t, 256> buffer{};
  auto n = port_.Read(buffer);
  std::size_t count = n.value_or(0);
  return assembler_.Feed(std::span<const std::uint8_t>(buffer.data(), count));
}

bool MavlinkLink::SendMessage(const mavlink_message_t& message) {
  std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> buffer{};
  std::uint16_t len = mavlink_msg_to_send_buffer(buffer.data(), &message);
  auto result = port_.Write(std::span<const std::uint8_t>(buffer.data(), len));
  return result.has_value() && *result == len;
}

}  // namespace uart
