/**
 * @file mavlink_link.cpp
 * @brief mavlink_link.hpp 的实现。
 */

#include "uart/mavlink_link.hpp"

#include <array>
#include <algorithm>

namespace uart {

std::optional<mavlink_message_t> MavlinkFrameAssembler::Feed(std::span<const std::uint8_t> bytes) {
  auto frame = FeedFrame(bytes, SteadyClock::now());
  if (!frame) return std::nullopt;
  return frame->message;
}

std::optional<WireFrame> MavlinkFrameAssembler::FeedFrame(
    std::span<const std::uint8_t> bytes, SteadyClock::time_point received_at) {
  for (std::uint8_t byte : bytes) {
    recent_bytes_.push_back(byte);
    if (recent_bytes_.size() > MAVLINK_MAX_PACKET_LEN) recent_bytes_.pop_front();
    mavlink_message_t out_msg{};
    mavlink_status_t out_status{};
    std::uint8_t result = mavlink_frame_char_buffer(&rx_msg_, &status_, byte, &out_msg, &out_status);
    if (result == MAVLINK_FRAMING_OK) {
      const std::size_t packet_size = out_msg.len +
          (out_msg.magic == MAVLINK_STX_MAVLINK1 ? 8u : 12u) +
          ((out_msg.incompat_flags & MAVLINK_IFLAG_SIGNED) ? MAVLINK_SIGNATURE_BLOCK_LEN : 0u);
      WireFrame frame{.message = out_msg, .bytes = {}, .received_at = received_at};
      if (recent_bytes_.size() >= packet_size &&
          recent_bytes_[recent_bytes_.size() - packet_size] == out_msg.magic) {
        auto first = recent_bytes_.end() - static_cast<std::ptrdiff_t>(packet_size);
        frame.bytes.assign(first, recent_bytes_.end());
      }
      pending_.push(std::move(frame));
    }
  }

  if (pending_.empty()) {
    return std::nullopt;
  }
  WireFrame frame = std::move(pending_.front());
  pending_.pop();
  return frame;
}

std::expected<MavlinkLink, UartError> MavlinkLink::Open(const std::string& device, int baud) {
  auto port = SerialPort::Open(device, baud);
  if (!port) {
    return std::unexpected(port.error());
  }
  return MavlinkLink(std::move(*port));
}

std::expected<std::optional<mavlink_message_t>, UartError> MavlinkLink::ReceiveMessage() {
  auto frame = ReceiveFrame();
  if (!frame) return std::unexpected(frame.error());
  if (!*frame) return std::nullopt;
  return (*frame)->message;
}

std::expected<std::optional<WireFrame>, UartError> MavlinkLink::ReceiveFrame() {
  if (auto pending = assembler_.FeedFrame({}, SteadyClock::now())) return pending;
  std::array<std::uint8_t, 256> buffer{};
  auto count = port_.Read(buffer);
  if (!count) {
    return std::unexpected(count.error());
  }
  return assembler_.FeedFrame(std::span<const std::uint8_t>(buffer.data(), *count),
                              SteadyClock::now());
}

std::expected<std::optional<mavlink_message_t>, UartError>
MavlinkLink::ReceiveMessage(std::chrono::milliseconds max_wait) {
  auto frame = ReceiveFrame(max_wait);
  if (!frame) return std::unexpected(frame.error());
  if (!*frame) return std::nullopt;
  return (*frame)->message;
}

std::expected<std::optional<WireFrame>, UartError>
MavlinkLink::ReceiveFrame(std::chrono::milliseconds max_wait) {
  if (auto pending = assembler_.FeedFrame({}, SteadyClock::now())) return pending;
  auto readable = port_.WaitReadable(max_wait);
  if (!readable) {
    return std::unexpected(readable.error());
  }
  if (!*readable) {
    return std::nullopt;
  }
  return ReceiveFrame();
}

std::expected<void, UartError> MavlinkLink::SendMessage(const mavlink_message_t& message) {
  auto sent = SendFrame(message);
  if (!sent) return std::unexpected(sent.error());
  return {};
}

std::expected<SentFrame, UartError> MavlinkLink::SendFrame(const mavlink_message_t& message) {
  std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> buffer{};
  const std::uint16_t length = mavlink_msg_to_send_buffer(buffer.data(), &message);
  SentFrame frame{.bytes = std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + length),
                  .started_at = SteadyClock::now()};
  auto written = port_.Write(std::span<const std::uint8_t>(buffer.data(), length));
  if (!written) {
    return std::unexpected(written.error());
  }
  if (*written != length) {
    return std::unexpected(UartError::kWriteError);
  }
  return frame;
}

std::expected<int, UartError> MavlinkLink::AppliedBaud() const {
  return port_.AppliedBaud();
}

}  // namespace uart
