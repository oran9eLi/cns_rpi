#pragma once

/**
 * @file link_lab.hpp
 * @brief 链路排障实验的进程内临时覆盖、会话栅栏与设备 ACK。
 *
 * 临时波特率和教学 5G 状态不会写入业务配置；重启后自动回到持久配置。
 */

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "cellular/cellular_status.hpp"
#include "experiment/pi_link_inspection.hpp"

namespace experiment {

bool IsLinkLabOperation(std::string_view payload);

struct LinkLabActions {
  /// true 仅表示 RPICELL 帧已完整写入当前唯一串口，不代表 F407 已显示。
  std::function<bool(bool force_offline)> send_cellular;
  /// 关闭并重新打开当前唯一串口，返回 termios 读回波特率。
  std::function<std::optional<int>(int baud)> apply_baud;
};

class LinkLabController {
 public:
  LinkLabController(std::string topic_namespace, std::string device_id,
                    int configured_baud, std::filesystem::path fence_file = {});

  bool Ready() const { return ready_; }
  const std::string& DeviceId() const { return device_id_; }
  void SetConfiguredBaud(int baud) { configured_baud_ = baud; }
  int CurrentBaud() const { return temporary_baud_.value_or(configured_baud_); }
  bool CellularOffline() const { return cellular_offline_; }
  cellular::LinkStatus TeachingCellularStatus(cellular::LinkStatus actual) const;
  bool ReportingEnabled() const { return reporting_enabled_; }
  bool HasTemporaryOverride() const {
    return temporary_baud_.has_value() || cellular_offline_ || !reporting_enabled_;
  }
  bool SessionActive() const { return session_started_at_.has_value(); }
  bool SessionExpired(std::chrono::steady_clock::time_point now) const;
  /// MQTT 控制链中断或本机租约超时：清除覆盖并封闭旧会话。
  void Abort();

  Outcome Handle(std::string_view payload, const LinkLabActions& actions,
                 std::chrono::system_clock::time_point wall_now,
                 std::chrono::steady_clock::time_point steady_now =
                     std::chrono::steady_clock::now());

 private:
  struct Entry {
    std::string action_id;
    std::string comparison;
    std::string ack;
    std::chrono::steady_clock::time_point completed_at;
  };

  bool LoadFence();
  bool SaveFence(std::uint64_t lease, std::string_view session);

  std::string topic_namespace_;
  std::string device_id_;
  int configured_baud_{};
  std::filesystem::path fence_file_;
  bool ready_{true};
  std::uint64_t lease_version_{};
  std::string session_id_;
  bool previous_process_session_{false};
  std::optional<int> temporary_baud_;
  bool cellular_offline_{false};
  bool reporting_enabled_{true};
  std::optional<std::chrono::steady_clock::time_point> session_started_at_;
  std::deque<Entry> entries_;
};

}  // namespace experiment
