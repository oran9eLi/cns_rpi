/** @file raw_frame.cpp @brief 有界重组原帧并核对运输 CRC，保留源校验失败报文。 */
#include "gnss/raw_frame.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace gnss {
namespace {

using namespace std::chrono_literals;
constexpr auto kLifetime = 5s;
constexpr std::size_t kMaxPartials = 32;
constexpr std::size_t kMaxTombstones = 64;
constexpr std::size_t kHeaderLength = 24;
constexpr std::size_t kFragmentLength = 40;

struct Fragment {
  RawFrame frame;
  std::uint16_t frame_length{}, offset{};
  std::uint8_t index{}, count{}, data_length{};
  std::array<std::uint8_t, kFragmentLength> data{};
};

std::uint16_t Read16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

std::uint32_t Read32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::optional<Fragment> Decode(const mavlink_message_t& message,
                               const mavlink_tunnel_t& tunnel) {
  const auto* bytes = tunnel.payload;
  if (tunnel.target_system != 0 || tunnel.target_component != 0 ||
      tunnel.payload_length < kHeaderLength + 1 ||
      tunnel.payload_length > kHeaderLength + kFragmentLength ||
      bytes[0] != 1 || bytes[3] != kHeaderLength ||
      (bytes[1] != 1 && bytes[1] != 2) ||
      (bytes[2] != 2 && bytes[2] != 3)) {
    return std::nullopt;
  }

  Fragment fragment;
  fragment.frame.system_id = message.sysid;
  fragment.frame.component_id = message.compid;
  fragment.frame.protocol = bytes[1];
  fragment.frame.flags = bytes[2];
  fragment.frame.frame_id = Read32(bytes + 4);
  fragment.frame.received_uptime_ms = Read32(bytes + 8);
  fragment.frame_length = Read16(bytes + 12);
  fragment.offset = Read16(bytes + 14);
  fragment.index = bytes[16];
  fragment.count = bytes[17];
  fragment.data_length = bytes[18];
  fragment.frame.nmea_type = bytes[19];
  fragment.frame.ubx_class = bytes[20];
  fragment.frame.ubx_id = bytes[21];
  fragment.frame.wire_crc16 = Read16(bytes + 22);

  const auto max_length = fragment.frame.protocol == 1 ? 128 : 1024;
  if (fragment.frame_length == 0 || fragment.frame_length > max_length ||
      fragment.count != (fragment.frame_length + kFragmentLength - 1) / kFragmentLength ||
      fragment.index >= fragment.count ||
      fragment.offset != fragment.index * kFragmentLength ||
      fragment.data_length != std::min<std::size_t>(
          kFragmentLength, fragment.frame_length - fragment.offset) ||
      tunnel.payload_length != kHeaderLength + fragment.data_length) {
    return std::nullopt;
  }
  if ((fragment.frame.protocol == 1 &&
       (fragment.frame.nmea_type > 4 || fragment.frame.ubx_class != 0 ||
        fragment.frame.ubx_id != 0)) ||
      (fragment.frame.protocol == 2 && fragment.frame.nmea_type != 0)) {
    return std::nullopt;
  }
  std::copy_n(bytes + kHeaderLength, fragment.data_length, fragment.data.begin());
  return fragment;
}

bool SameMetadata(const RawFrame& existing, std::uint8_t existing_count,
                  const Fragment& fragment) {
  const auto& incoming = fragment.frame;
  return existing.protocol == incoming.protocol &&
         existing.flags == incoming.flags &&
         existing.nmea_type == incoming.nmea_type &&
         existing.ubx_class == incoming.ubx_class &&
         existing.ubx_id == incoming.ubx_id &&
         existing.wire_crc16 == incoming.wire_crc16 &&
         existing.wire.size() == fragment.frame_length &&
         existing_count == fragment.count;
}

std::uint16_t WireCrc(std::span<const std::uint8_t> wire) {
  std::uint16_t crc = 0xFFFF;
  for (const auto byte : wire) {
    crc ^= static_cast<std::uint16_t>(byte << 8);
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = static_cast<std::uint16_t>((crc & 0x8000) != 0
                                          ? (crc << 1) ^ 0x1021 : crc << 1);
    }
  }
  return crc;
}

}  // namespace

std::optional<RawFrame> Assembler::Add(const mavlink_message_t& message,
                                     Clock::time_point now) {
  Tick(now);
  if (message.msgid != MAVLINK_MSG_ID_TUNNEL || message.compid != 193 ||
      message.sysid == 0 || message.sysid > 250) {
    return std::nullopt;
  }
  mavlink_tunnel_t tunnel{};
  // 官方解码恢复 MAVLink2 尾零截短，不能按 message.len 裁剪原文。
  mavlink_msg_tunnel_decode(&message, &tunnel);
  if (tunnel.payload_type != 0x8020) return std::nullopt;
  ++counters_.fragments_received;
  auto fragment = Decode(message, tunnel);
  if (!fragment) {
    ++counters_.invalid_fragments;
    return std::nullopt;
  }
  const Key key{message.sysid, message.compid, fragment->frame.frame_id,
                fragment->frame.received_uptime_ms};
  const auto retired = std::find_if(tombstones_.begin(), tombstones_.end(),
      [&](const auto& entry) { return entry.key == key; });
  if (retired != tombstones_.end()) {
    if (retired->completed) ++counters_.duplicate_frames;
    return std::nullopt;
  }

  auto partial = std::find_if(partials_.begin(), partials_.end(),
      [&](const auto& entry) { return entry.key == key; });
  if (partial != partials_.end() &&
      !SameMetadata(partial->frame, partial->fragment_count, *fragment)) {
    ++counters_.conflicting_frames;
    Retire(key, false, now);
    partials_.erase(partial);
    return std::nullopt;
  }
  if (partial == partials_.end()) {
    if (partials_.size() == kMaxPartials) {
      ++counters_.evicted_frames;
      Retire(partials_.front().key, false, now);
      partials_.erase(partials_.begin());
    }
    fragment->frame.wire.resize(fragment->frame_length);
    partials_.push_back(Partial{key, std::move(fragment->frame), fragment->count,
                                0, now});
    partial = std::prev(partials_.end());
  }

  const auto destination = partial->frame.wire.begin() + fragment->offset;
  const auto mask = std::uint32_t{1} << fragment->index;
  if ((partial->received_mask & mask) != 0) {
    if (std::equal(fragment->data.begin(),
                   fragment->data.begin() + fragment->data_length, destination)) {
      ++counters_.duplicate_fragments;
    } else {
      ++counters_.conflicting_frames;
      Retire(key, false, now);
      partials_.erase(partial);
    }
    return std::nullopt;
  }
  std::copy_n(fragment->data.begin(), fragment->data_length, destination);
  partial->received_mask |= mask;
  if (partial->received_mask != (std::uint32_t{1} << partial->fragment_count) - 1) {
    return std::nullopt;
  }

  const bool valid_crc = WireCrc(partial->frame.wire) == partial->frame.wire_crc16;
  std::optional<RawFrame> result;
  if (valid_crc) {
    ++counters_.frames_completed;
    result = std::move(partial->frame);
  } else {
    ++counters_.wire_crc_errors;
  }
  Retire(key, valid_crc, now);
  partials_.erase(partial);
  return result;
}

void Assembler::Tick(Clock::time_point now) {
  std::erase_if(tombstones_, [&](const auto& entry) { return entry.expires_at <= now; });
  for (auto partial = partials_.begin(); partial != partials_.end();) {
    if (now - partial->started_at >= kLifetime) {
      ++counters_.timed_out_frames;
      Retire(partial->key, false, now);
      partial = partials_.erase(partial);
    } else {
      ++partial;
    }
  }
}

void Assembler::Reset() {
  partials_.clear();
  tombstones_.clear();
}

void Assembler::Retire(const Key& key, bool completed, Clock::time_point now) {
  if (tombstones_.size() == kMaxTombstones) tombstones_.pop_front();
  tombstones_.push_back(Tombstone{key, completed, now + kLifetime});
}

}  // namespace gnss
