#pragma once

/**
 * @file runtime_status.hpp
 * @brief 主控箱内部运行状态模型和业务静默计时。
 *
 * @details
 * 本模块只跟踪真实身份和有效业务帧事实，供设备会话、命令门禁和实验观测使用，
 * 不读取串口、不建立 MQTT 连接，也不构造 MQTT 状态载荷。
 */

#include <chrono>
#include <optional>
#include <string>

namespace runtime_status {

using Clock = std::chrono::steady_clock;

enum class ControlStatus { kOnline, kOffline };
enum class BusinessStatus { kOnline, kOffline, kUnknown };
enum class IdentityStatus { kVerified, kCached, kConflict, kUnbound };

/// 一份设备会话内部状态，供身份门禁和实验观测读取。
struct Snapshot {
  std::string device_id;
  ControlStatus control_status{ControlStatus::kOnline};
  BusinessStatus business_status{BusinessStatus::kUnknown};
  IdentityStatus identity_status{IdentityStatus::kUnbound};

  bool operator==(const Snapshot&) const = default;
};

/**
 * @brief 跟踪一台主控箱本次进程内的业务和身份事实。
 * @details 本进程供本地门禁和实验观测读取的快照中，控制链状态恒为 online。
 * 对外 MQTT 在线状态由独立的设备状态上报模块表达。
 */
class Tracker {
 public:
  Tracker(std::string device_id,
          std::optional<std::string> persisted_device_id,
          Clock::time_point started_at);

  /// 记录一条由现有解码器确认的 F407 有效业务帧。
  void ObserveBusinessFrame(Clock::time_point now);

  /// 记录从真实 OPEN_DRONE_ID_BASIC_ID 提取并校验通过的当前身份。
  void ObserveIdentity(const std::string& current_device_id);

  /// 首次绑定原子落盘成功后，立即把本次真实身份标记为已验证。
  void ConfirmPersistedIdentity(const std::string& persisted_device_id);

  /// 当前串口会话失效后撤销实时核验结论，仅保留缓存或未绑定事实。
  void InvalidateCurrentIdentity();

  /// 推进固定 10 秒静默判定；调用频率不会改变状态语义。
  void Tick(Clock::time_point now);

  Snapshot CurrentOnlineSnapshot() const;

 private:
  std::string device_id_;
  std::optional<std::string> persisted_device_id_;
  Clock::time_point started_at_;
  std::optional<Clock::time_point> last_business_frame_;
  BusinessStatus business_status_{BusinessStatus::kUnknown};
  IdentityStatus identity_status_{IdentityStatus::kUnbound};
};

}  // namespace runtime_status
