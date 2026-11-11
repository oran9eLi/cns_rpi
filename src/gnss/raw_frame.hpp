#pragma once

/** @file raw_frame.hpp @brief F407 0x8020 分片恢复；不解析定位语义、不持有串口。 */

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "common/mavlink.h"

namespace gnss {

struct RawFrame {
  std::uint8_t system_id{}, component_id{}, protocol{}, flags{}, nmea_type{},
      ubx_class{}, ubx_id{};
  // frame_id 可回绕；received_uptime_ms 是 F407 启动毫秒数，不能当 UTC。
  std::uint32_t frame_id{}, received_uptime_ms{};
  std::uint16_t wire_crc16{};
  std::vector<std::uint8_t> wire;
};

struct Counters {
  std::uint64_t fragments_received{}, frames_completed{}, invalid_fragments{},
      timed_out_frames{}, conflicting_frames{}, wire_crc_errors{},
      duplicate_fragments{}, duplicate_frames{}, evicted_frames{};
};

class Assembler {
 public:
  using Clock = std::chrono::steady_clock;
  /// 由唯一串口读者在已核验来源后调用；只返回完整且 wire CRC 正确的帧。
  std::optional<RawFrame> Add(const mavlink_message_t& message,
                            Clock::time_point now);
  void Tick(Clock::time_point now);
  void Reset();  ///< 清除未齐组和短期去重键，保留本进程累计计数。
  const Counters& Stats() const { return counters_; }

 private:
  struct Key {
    std::uint8_t system_id{}, component_id{};
    std::uint32_t frame_id{}, received_uptime_ms{};
    bool operator==(const Key&) const = default;
  };
  struct Partial {
    Key key;
    RawFrame frame;
    std::uint8_t fragment_count{};
    std::uint32_t received_mask{};
    Clock::time_point started_at;
  };
  struct Tombstone {
    Key key;
    bool completed{};
    Clock::time_point expires_at;
  };

  void Retire(const Key& key, bool completed, Clock::time_point now);

  Counters counters_;
  std::vector<Partial> partials_;
  std::deque<Tombstone> tombstones_;
};

}  // namespace gnss
