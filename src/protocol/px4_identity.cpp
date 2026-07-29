#include "protocol/px4_identity.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>

namespace protocol {

namespace {

bool HasUid2(const std::uint8_t (&uid2)[18]) {
  return std::any_of(std::begin(uid2), std::end(uid2),
                     [](std::uint8_t value) { return value != 0; });
}

std::string FormatUid2Bytes(const std::uint8_t (&uid2)[18]) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string result{"PX4U2-"};
  result.reserve(6 + 36);
  for (const std::uint8_t value : uid2) {
    result.push_back(kHex[value >> 4]);
    result.push_back(kHex[value & 0x0F]);
  }
  return result;
}

}  // namespace

std::optional<std::string> FormatPx4Uid2(
    const mavlink_autopilot_version_t& version) {
  if (!HasUid2(version.uid2)) {
    return std::nullopt;
  }
  const std::string device_id = FormatUid2Bytes(version.uid2);
  return device_id.substr(6);
}

std::optional<std::string> FormatPx4Uid(
    const mavlink_autopilot_version_t& version) {
  if (version.uid == 0) {
    return std::nullopt;
  }
  std::array<char, 17> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%016llX",
                static_cast<unsigned long long>(version.uid));
  return std::string{buffer.data()};
}

std::optional<std::string> FormatPx4DeviceId(
    const mavlink_autopilot_version_t& version) {
  if (const auto uid2 = FormatPx4Uid2(version)) {
    return "PX4U2-" + *uid2;
  }
  if (const auto uid = FormatPx4Uid(version)) {
    return "PX4U1-" + *uid;
  }
  return std::nullopt;
}

mavlink_message_t BuildAutopilotVersionRequest(
    std::uint8_t source_system_id,
    std::uint8_t source_component_id,
    const control_command::MavlinkEndpoint& target) {
  mavlink_message_t message{};
  mavlink_msg_command_long_pack(
      source_system_id, source_component_id, &message, target.system_id,
      target.component_id, MAV_CMD_REQUEST_MESSAGE, 0,
      static_cast<float>(MAVLINK_MSG_ID_AUTOPILOT_VERSION), 0.0F, 0.0F, 0.0F,
      0.0F, 0.0F, 0.0F);
  return message;
}

}  // namespace protocol
