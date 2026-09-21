#pragma once

/**
 * @file device_session.hpp
 * @brief 解耦持久化设备身份、当前 F407 链路和 MQTT 控制会话所需事实。
 *
 * @details
 * 本模块不打开串口、不读写绑定文件、不发布 MQTT。它只保证缓存身份不会因串口
 * 断开而丢失、冲突身份不会自动改绑，并向组合根提供动作和遥测是否安全的判定。
 */

#include <optional>

#include "device/device_binding_store.hpp"
#include "runtime_status/runtime_status.hpp"

namespace runtime_status {

enum class IdentityObservation {
  kVerified,  ///< 当前真实身份与持久化绑定一致。
  kConflict,  ///< 当前真实身份与持久化绑定冲突。
  kUnbound,   ///< 已取得真实身份，但尚无合法持久化绑定。
};

class DeviceSession {
 public:
  DeviceSession(std::optional<device::Binding> persisted_binding,
                Clock::time_point started_at);

  /// 返回 MQTT Topic 应使用的稳定身份；无缓存且尚未取得真实身份时为空。
  const device::Binding* ActiveBinding() const;
  bool HasPersistedBinding() const;

  /**
   * @brief 核对真实 F407 身份。
   * @details 无绑定时锁定首个真实身份但保持 unbound；有绑定时只核对，不改写。
   */
  IdentityObservation ObserveIdentity(const device::Binding& current);

  /// BindOrVerify 成功后确认当前活动身份已经持久化。
  void ConfirmPersistedBinding();

  void SetLinkAvailable(bool available);
  void ObserveBusinessFrame(Clock::time_point now);
  void Tick(Clock::time_point now);

  std::optional<Snapshot> CurrentOnlineStatus() const;
  bool CanSendDeviceCommands() const;
  bool CanPublishTelemetry() const;

 private:
  Clock::time_point started_at_;
  std::optional<device::Binding> active_binding_;
  bool has_persisted_binding_{false};
  bool link_available_{false};
  std::optional<Tracker> tracker_;
};

}  // namespace runtime_status
