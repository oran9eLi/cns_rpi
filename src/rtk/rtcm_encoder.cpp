/** @file rtcm_encoder.cpp
 * @brief 使用官方 GPS_RTCM_DATA 生成头，保留接收机字节流顺序。
 */
#include "rtk/rtcm_encoder.hpp"

#include <algorithm>
#include <array>
namespace rtk {
std::vector<mavlink_message_t> EncodeRtcm(std::span<const std::uint8_t> bytes,
                                          std::uint8_t &sequence) {
  std::vector<mavlink_message_t> result;
  while (!bytes.empty()) {
    const auto group = bytes.first(std::min<std::size_t>(720, bytes.size()));
    const bool fragmented = group.size() > 180;
    const auto count =
        fragmented ? std::min<std::size_t>(4, group.size() / 180 + 1) : 1;
    for (std::size_t fragment = 0; fragment < count; ++fragment) {
      const auto offset = fragment * 180;
      const auto length = std::min<std::size_t>(180, group.size() - offset);
      std::array<std::uint8_t, 180> payload{};
      std::copy_n(group.begin() + offset, length, payload.begin());
      const auto flags = static_cast<std::uint8_t>(
          ((sequence & 31) << 3) | (fragmented ? 1 | (fragment << 1) : 0));
      mavlink_message_t message{};
      mavlink_msg_gps_rtcm_data_pack(255, MAV_COMP_ID_MISSIONPLANNER, &message,
                                     flags, static_cast<std::uint8_t>(length),
                                     payload.data());
      result.push_back(message);
    }
    sequence = (sequence + 1) & 31;
    bytes = bytes.subspan(group.size());
  }
  return result;
}
}  // namespace rtk
