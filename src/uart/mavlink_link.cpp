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

std::expected<std::optional<mavlink_message_t>, UartError> MavlinkLink::ReceiveMessage() {
  std::array<std::uint8_t, 256> buffer{};
  auto count = port_.Read(buffer);
  if (!count) {
    return std::unexpected(count.error());
  }
  return assembler_.Feed(std::span<const std::uint8_t>(buffer.data(), *count));
}

std::expected<void, UartError> MavlinkLink::SendMessage(const mavlink_message_t& message) {
  std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> buffer{};
  const std::uint16_t length = mavlink_msg_to_send_buffer(buffer.data(), &message);
  auto written = port_.Write(std::span<const std::uint8_t>(buffer.data(), length));
  if (!written) {
    return std::unexpected(written.error());
  }
  if (*written != length) {
    return std::unexpected(UartError::kWriteError);
  }
  return {};
}

}  // namespace uart
