/**
 * @file cellular_status.cpp
 * @brief 移动网络链路状态探测与心跳编码实现。
 */

#include "cellular/cellular_status.hpp"

#include <array>
#include <string>

namespace cellular {

namespace {

constexpr std::int32_t kOnlineBit = 1 << 0;
constexpr std::int32_t kPresentBit = 1 << 1;
constexpr std::int32_t kCarrierBit = 1 << 2;
constexpr std::int32_t kIpBit = 1 << 3;
constexpr std::int32_t kDefaultRouteBit = 1 << 4;

}  // namespace

LinkStatus FromSnapshot(const StatusSnapshot& snapshot) {
  LinkStatus status;
  status.interface_present = snapshot.diagnostics.interface_present;
  status.carrier_up = snapshot.diagnostics.carrier_up;
  status.has_ip_address = snapshot.diagnostics.has_ip_address;
  status.has_default_route = snapshot.diagnostics.has_default_route;
  status.online = snapshot.link_state == LinkState::kOnline ||
                  snapshot.link_state == LinkState::kDegraded;
  return status;
}

std::int32_t PackRpiCellularValue(const LinkStatus& status) {
  std::int32_t value = 0;
  if (status.online) {
    value |= kOnlineBit;
  }
  if (status.interface_present) {
    value |= kPresentBit;
  }
  if (status.carrier_up) {
    value |= kCarrierBit;
  }
  if (status.has_ip_address) {
    value |= kIpBit;
  }
  if (status.has_default_route) {
    value |= kDefaultRouteBit;
  }
  return value;
}

mavlink_message_t BuildRpiCellularHeartbeat(const LinkStatus& status, std::uint8_t system_id,
                                            std::uint8_t component_id,
                                            std::uint32_t time_boot_ms) {
  mavlink_message_t msg{};
  std::array<char, 10> name{};
  const char literal[] = "RPICELL";
  for (std::size_t i = 0; i < sizeof(literal) - 1; ++i) {
    name[i] = literal[i];
  }
  mavlink_msg_named_value_int_pack(system_id, component_id, &msg, time_boot_ms, name.data(),
                                   PackRpiCellularValue(status));
  return msg;
}

}  // namespace cellular
