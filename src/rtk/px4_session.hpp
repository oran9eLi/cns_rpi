#pragma once

/**
 * @file px4_session.hpp
 * @brief RTK 实验独立的 PX4 身份与遥测会话，不参与默认 F407 流程。
 *
 * 由 RTK 串口线程串行调用；只返回待发送 MAVLink 消息，由组合根统一写串口。
 * 不读取飞控 UID、不创建 Pi 身份、不转发外部飞控命令。
 */

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <deque>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "state/state_store.hpp"

namespace rtk {

/// 经 PX4 autopilot HEARTBEAT 选定的 MAVLink 寻址，不能作为业务身份。
struct Px4Endpoint {
  std::uint8_t system_id;
  std::uint8_t component_id;
};

/**
 * @brief RTK 组合根的 PX4 会话；全部方法在同一串口线程调用。
 *
 * 首个有效 Basic ID 锁定进程内业务身份；失联清空实时状态，重连仍须核对同一
 * Basic ID。不同身份/其他 system 不得自动改绑，需重新启动实验处理换机。
 */
class Px4Session {
 public:
  using Clock = std::chrono::steady_clock;

  /// sender ID 仅用于 MAVLink 帧头；heartbeat_timeout 为最后飞控心跳的有效期。
  explicit Px4Session(
      std::uint8_t sender_system_id = 255,
      std::uint8_t sender_component_id = MAV_COMP_ID_MISSIONPLANNER,
      std::chrono::milliseconds heartbeat_timeout = std::chrono::seconds(5));

  /// 入口仅接收已由官方解析器完成帧校验的消息；返回是否属于有效当前会话。
  bool Observe(const mavlink_message_t& message, Clock::time_point now, std::span<const std::uint8_t> wire = {});
  /// 时间窗口只筛选状态变化；after为开区间，through为闭区间，不影响连续GPS。
  [[nodiscard]] nlohmann::json GpsObservation(
      Clock::time_point now, std::optional<Clock::time_point> after = {},
      std::optional<Clock::time_point> through = {}) const;
  [[nodiscard]] nlohmann::json FirmwareObservation() const;
  /// 主循环即使没有新消息也必须调用，以清除心跳超时的身份和遥测。
  void Tick(Clock::time_point now);
  /// 串口关闭时立即撤销实时身份，保留已锁定身份与端点以防自动换机。
  void Invalidate();
  /// 已实时核验的业务主键；上层应在每轮先 Tick() 再读取。
  [[nodiscard]] std::optional<std::string> Identity() const;
  /// 返回可直接交给既有遥测序列化器的独立状态快照。
  [[nodiscard]] state::TelemetryState Snapshot() const;
  /// 已发现的 PX4 端点；存在端点不代表业务身份已经核验。
  [[nodiscard]] std::optional<Px4Endpoint> Endpoint() const;
  /// 只有未超时的飞控心跳与本次连接已核验身份同时存在时在线。
  [[nodiscard]] bool Online(Clock::time_point now) const;
  /// 最后一个当前会话有效帧的年龄；从未收到或已失效时为空，单位毫秒。
  [[nodiscard]] std::optional<std::chrono::milliseconds> LastFrameAge(
      Clock::time_point now) const;
  /// 每秒 GCS 心跳；待核验时每两秒请求 Basic ID，核验后每连接请求一次 GPS 速率与版本。
  [[nodiscard]] std::vector<mavlink_message_t> PollOutbound(Clock::time_point now);

 private:
  std::uint8_t sender_system_id_;
  std::uint8_t sender_component_id_;
  std::chrono::milliseconds heartbeat_timeout_;
  std::optional<Px4Endpoint> endpoint_;
  std::optional<std::string> bound_device_id_;
  std::optional<Clock::time_point> last_heartbeat_;
  std::optional<Clock::time_point> last_frame_;
  std::optional<Clock::time_point> last_gcs_heartbeat_;
  std::optional<Clock::time_point> last_identity_request_;
  void ObserveGps(const mavlink_message_t& message, Clock::time_point now,
                  std::span<const std::uint8_t> wire);
  std::optional<mavlink_gps_raw_int_t> gps_;
  Clock::time_point gps_frame_time_{}, gps_sample_time_{};
  std::uint64_t gps_sequence_{0}, transitions_evicted_{0};
  std::string gps_raw_hex_;
  struct Transition { std::uint64_t sequence; std::uint8_t fix; Clock::time_point time; };
  std::deque<Transition> transitions_;
  std::optional<mavlink_autopilot_version_t> firmware_;
  bool observation_requested_{false};
  state::StateStore store_;
};

}  // namespace rtk
