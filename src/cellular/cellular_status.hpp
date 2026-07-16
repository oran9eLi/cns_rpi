#pragma once

/**
 * @file cellular_status.hpp
 * @brief Probe the Raspberry Pi cellular link and encode it for the MCU.
 *
 * The MCU only needs a compact heartbeat, but the individual bits are kept so
 * bench logs can explain why the link is considered offline.
 */

#include <cstdint>
#include <string>
#include <string_view>

#include "common/mavlink.h"

namespace cellular {

struct LinkStatus {
  bool interface_present = false;
  bool carrier_up = false;
  bool has_ip_address = false;
  bool has_default_route = false;
  bool online = false;
};

LinkStatus ProbeLink(std::string_view interface_name);

std::int32_t PackRpiCellularValue(const LinkStatus& status);

mavlink_message_t BuildRpiCellularHeartbeat(const LinkStatus& status, std::uint8_t system_id,
                                            std::uint8_t component_id,
                                            std::uint32_t time_boot_ms);

}  // namespace cellular
