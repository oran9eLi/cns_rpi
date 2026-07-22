#pragma once

/**
 * @file cellular_status.hpp
 * @brief 探测树莓派移动网络链路状态，并编码后发送给单片机。
 *
 * 单片机只需要紧凑的心跳值，同时保留各项状态位，便于联调时定位离线原因。
 */

#include <cstdint>
#include <string>
#include <string_view>

#include "cellular/cellular_snapshot.hpp"
#include "common/mavlink.h"

namespace cellular {

struct LinkStatus {
  bool interface_present = false;
  bool carrier_up = false;
  bool has_ip_address = false;
  bool has_default_route = false;
  bool online = false;
};

LinkStatus FromSnapshot(const StatusSnapshot& snapshot);

std::int32_t PackRpiCellularValue(const LinkStatus& status);

mavlink_message_t BuildRpiCellularHeartbeat(const LinkStatus& status, std::uint8_t system_id,
                                            std::uint8_t component_id,
                                            std::uint32_t time_boot_ms);

}  // namespace cellular
