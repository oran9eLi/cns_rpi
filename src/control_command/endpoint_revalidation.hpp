#pragma once

/**
 * @file endpoint_revalidation.hpp
 * @brief F407 同一串口会话中可变 MAVLink 端点的候选身份复核。
 *
 * @details 本模块不持有串口、不写绑定文件、不发布 MQTT。
 */

#include <chrono>
#include <optional>
#include <string_view>

#include "control_command/control_endpoint.hpp"

namespace control_command {

enum class HeartbeatObservation { kUnchanged, kCandidate, kAmbiguous };
enum class BasicIdObservation { kIgnored, kVerified, kConflict };

/** @brief 身份失效时仅在非换号复核场景主动终止已发H1动作。 */
bool ShouldAbortEchoForIdentity(bool has_pending, bool identity_ready,
                                bool revalidation_pending);

class EndpointRevalidation {
 public:
  using Clock = std::chrono::steady_clock;

  /** @brief 观察已分类的心跳；不直接改写当前受控端点。 */
  HeartbeatObservation ObserveHeartbeat(
      const ControlledDeviceEndpoint& current,
      const ControlledDeviceEndpoint& observed, bool has_binding,
      Clock::time_point now);

  /** @brief 只核验当前候选端点的 Basic ID。 */
  BasicIdObservation ObserveBasicId(const mavlink_message_t& message,
                                    std::string_view bound_device_id);

  bool Pending() const;
  std::optional<MavlinkEndpoint> Candidate() const;
  bool ShouldWarn(Clock::time_point now);
  void Reset();

 private:
  std::optional<MavlinkEndpoint> candidate_;
  Clock::time_point started_at_{};
  bool ambiguous_{};
  bool conflict_{};
  bool warned_{};
};

}  // namespace control_command
