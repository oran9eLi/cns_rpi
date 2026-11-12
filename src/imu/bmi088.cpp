#include "imu/bmi088.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <span>

namespace imu {
namespace {
using Bytes = std::array<std::uint8_t, 64>;
std::uint16_t U16(const Bytes& p, int offset);
std::uint32_t U32(const Bytes& p, int offset);
std::int16_t I16(const Bytes& p, int offset);
std::int32_t I32(const Bytes& p, int offset);
std::expected<Raw, std::string_view> DecodeRaw(const Bytes& p, const Header& header);
std::expected<Parsed, std::string_view> DecodeParsed(const Bytes& p);
std::expected<Status, std::string_view> DecodeStatus(const Bytes& p, const Header& header);
}  // namespace

std::expected<std::optional<Packet>, std::string_view> Decode(const mavlink_message_t& message) {
  if (message.msgid != MAVLINK_MSG_ID_TUNNEL) return std::nullopt;
  mavlink_tunnel_t tunnel{};
  mavlink_msg_tunnel_decode(&message, &tunnel);
  if (tunnel.payload_type < 0x8030 || tunnel.payload_type > 0x8032) return std::nullopt;
  if (message.sysid < 1 || message.sysid > 250 || message.compid != 193)
    return std::unexpected("invalid BMI088 source endpoint");
  if (tunnel.target_system || tunnel.target_component || tunnel.payload_length != 64 ||
      !std::all_of(tunnel.payload + 64, tunnel.payload + 128, [](auto byte) { return byte == 0; }))
    return std::unexpected("invalid BMI088 TUNNEL envelope");
  Packet packet;
  packet.system_id = message.sysid;
  packet.component_id = message.compid;
  std::copy_n(tunnel.payload, 64, packet.payload.begin());
  const auto& p = packet.payload;
  packet.header = {p[1], p[2], p[3], U32(p, 4), U32(p, 8), U32(p, 12), U32(p, 16)};
  const auto& header = packet.header;
  if (p[0] != 1 || p[1] != tunnel.payload_type - 0x8030 || p[2] & 0xf0 ||
      p[3] > 2 || (p[3] == 0 && p[1] != 2))
    return std::unexpected("invalid BMI088 header");
  const auto age = header.encode_time - header.sample_time;
  if (header.Usable() && (!header.Present() || header.Stale() || header.Invalidated() || age > 200))
    return std::unexpected("inconsistent BMI088 usable flag");
  if (header.Present() && header.sequence == 0)
    return std::unexpected("BMI088 sequence zero is reserved for no sample");
  if (header.kind < 2 && !header.Present())
    return std::unexpected("BMI088 measurement has no sample");
  if (header.kind == 0) {
    auto raw = DecodeRaw(p, header);
    if (!raw) return std::unexpected(raw.error());
    packet.data = *raw;
  } else if (header.kind == 1) {
    auto parsed = DecodeParsed(p);
    if (!parsed) return std::unexpected(parsed.error());
    packet.data = *parsed;
  } else {
    auto status = DecodeStatus(p, header);
    if (!status) return std::unexpected(status.error());
    packet.data = *status;
  }
  return packet;
}

namespace {
std::uint16_t U16(const Bytes& p, int offset) {
  return static_cast<std::uint16_t>(p[offset]) | (static_cast<std::uint16_t>(p[offset + 1]) << 8);
}
std::uint32_t U32(const Bytes& p, int offset) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(p[offset + i]) << (8 * i);
  return value;
}
std::int16_t I16(const Bytes& p, int offset) { return std::bit_cast<std::int16_t>(U16(p, offset)); }
std::int32_t I32(const Bytes& p, int offset) { return std::bit_cast<std::int32_t>(U32(p, offset)); }

std::expected<Raw, std::string_view> DecodeRaw(const Bytes& p, const Header& header) {
  const bool addresses_ok = header.bus == 2 ? p[20] == 0 && p[21] == 0
      : (p[20] == 0x18 || p[20] == 0x19) && (p[21] == 0x68 || p[21] == 0x69);
  if (!addresses_ok || p[22] != 0x1e || p[23] != 0x0f || !(p[24] & 0x80) ||
      p[25] != p[26] || p[26] < 1 || p[26] > 4 || p[27] != 1 || p[37] == 0x80 || p[63])
    return std::unexpected("invalid BMI088 RAW profile or register facts");
  if (!std::all_of(p.begin() + 39 + p[26] * 6, p.begin() + 63,
                   [](auto byte) { return byte == 0; }))
    return std::unexpected("nonzero BMI088 RAW unused FIFO bytes");
  Raw raw;
  raw.frame_count = p[26];
  for (int axis = 0; axis < 3; ++axis) {
    raw.accel[axis] = I16(p, 28 + axis * 2);
    raw.gyro[axis] = I16(p, 39 + (p[26] - 1) * 6 + axis * 2);
  }
  int temperature = (p[37] << 3) | (p[38] >> 5);
  if (temperature >= 1024) temperature -= 2048;
  raw.temperature_cdeg = temperature * 12.5 + 2300;
  return raw;
}

std::expected<Parsed, std::string_view> DecodeParsed(const Bytes& p) {
  Parsed parsed;
  for (int axis = 0; axis < 3; ++axis) {
    parsed.accel_mg[axis] = I32(p, 20 + axis * 4);
    parsed.gyro_mdps[axis] = I32(p, 32 + axis * 4);
    parsed.axis_map[axis] = std::bit_cast<std::int8_t>(p[52 + axis]);
  }
  int orientation = 1;
  for (int i = 0; i < 3; ++i) {
    const int axis = parsed.axis_map[i];
    if (axis == 0 || std::abs(axis) > 3)
      return std::unexpected("invalid BMI088 axis map");
    if (axis < 0) orientation = -orientation;
    for (int j = 0; j < i; ++j) {
      if (std::abs(axis) == std::abs(parsed.axis_map[j]))
        return std::unexpected("duplicate BMI088 axis map");
      if (std::abs(parsed.axis_map[j]) > std::abs(axis)) orientation = -orientation;
    }
  }
  parsed.temperature_cdeg = I16(p, 44);
  parsed.sample_period_us = U16(p, 46);
  parsed.accel_fullscale_mg = U16(p, 48);
  parsed.gyro_fullscale_dps = U16(p, 50);
  parsed.fifo_drop_count = U32(p, 56);
  parsed.driver_error_count = U32(p, 60);
  if (orientation != 1 || p[55] != 1 || parsed.accel_fullscale_mg != 12000 ||
      parsed.gyro_fullscale_dps != 1000 || parsed.sample_period_us == 0)
    return std::unexpected("invalid BMI088 PARSED profile or handedness");
  return parsed;
}

std::expected<Status, std::string_view> DecodeStatus(const Bytes& p, const Header& header) {
  if (p[48] > 9 || p[49] > 6 || p[50] > 3 || p[51] > 1 || p[54] > 127 ||
      p[55] > 127 || p[60] > 1 || p[61] > 1 || (p[62] > 6 && p[62] != 255) || p[63] ||
      (header.bus == 2 && (p[54] || p[55])))
    return std::unexpected("invalid BMI088 STATUS fields");
  return Status{U32(p, 20), U32(p, 24), U32(p, 28), U32(p, 32), U32(p, 36), U32(p, 40),
      U32(p, 44), p[48], p[49], p[50], p[51], p[52], p[53], p[54], p[55], U32(p, 56),
      p[60] != 0, p[61] != 0, p[62]};
}
}  // namespace
}  // namespace imu
