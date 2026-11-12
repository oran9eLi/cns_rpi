#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "state/module_status.hpp"

namespace device_status {

using HealthClock = std::chrono::steady_clock;

/// 主循环按当前绑定身份调用；只记录运行时长，不保存可补算关机时间的墙钟时间。
class HealthTracker {
 public:
  explicit HealthTracker(std::filesystem::path state_file);

  bool Load(const std::string& device_id, std::string* error);
  void Update(
      const std::optional<std::array<std::uint8_t, state::kModuleCount>>& modules,
      HealthClock::time_point now);
  bool Save(std::string* error) const;
  /// 供后续云端管理入口使用；持久化失败时保留原维修状态。
  bool SetMaintenance(bool maintenance, std::string* error);
  std::string Status() const;
  std::optional<std::vector<std::string>> FaultModules() const;

 private:
  std::filesystem::path state_file_;
  std::string device_id_;
  std::array<HealthClock::duration, state::kModuleCount> failure_time_{};
  std::array<bool, state::kModuleCount> failing_{};
  bool modules_available_{false};
  std::optional<HealthClock::time_point> last_update_;
  bool maintenance_{false};
};

}  // namespace device_status
