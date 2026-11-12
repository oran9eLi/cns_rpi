#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <variant>

#include "common/mavlink.h"

namespace imu {

struct Header {
  std::uint8_t kind{}, flags{}, bus{};
  std::uint32_t sequence{}, sample_time{}, encode_time{}, reinit_count{};
  bool Present() const { return flags & 1; }
  bool Usable() const { return flags & 2; }
  bool Stale() const { return flags & 4; }
  bool Invalidated() const { return flags & 8; }
};

struct Raw {
  std::array<std::int16_t, 3> accel{}, gyro{};
  double temperature_cdeg{};
  std::uint8_t frame_count{};
};

struct Parsed {
  std::array<std::int32_t, 3> accel_mg{}, gyro_mdps{};
  std::int16_t temperature_cdeg{};
  std::uint16_t sample_period_us{}, accel_fullscale_mg{}, gyro_fullscale_dps{};
  std::array<std::int8_t, 3> axis_map{};
  std::uint32_t fifo_drop_count{}, driver_error_count{};
};

struct Status {
  std::uint32_t driver_error_count{}, fifo_drop_count{}, source_published_count{},
      source_overwrites{}, completed_pairs{}, abandoned_pairs{}, tx_failures{};
  std::uint8_t driver_stage{}, driver_result{}, bsp_result{}, device_ready{},
      accel_chip_id{}, gyro_chip_id{}, accel_addr{}, gyro_addr{};
  std::uint32_t source_age_ms{};
  bool diagnostics_available{}, source_available{};
  std::uint8_t framework_state{};
};

struct Packet {
  std::uint8_t system_id{}, component_id{};
  Header header;
  std::array<std::uint8_t, 64> payload{};
  std::variant<Raw, Parsed, Status> data;
};

// The sole UART assembler has already verified the MAVLink CRC.
std::expected<std::optional<Packet>, std::string_view> Decode(const mavlink_message_t& message);

}  // namespace imu
