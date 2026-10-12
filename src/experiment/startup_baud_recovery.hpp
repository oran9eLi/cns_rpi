#pragma once

/**
 * @file startup_baud_recovery.hpp
 * @brief 主控箱进程启动后的串口速率恢复状态。
 *
 * @details 仅调度探测速率，不打开串口或发送帧；运行中的 M1 动作可禁用恢复。
 */

#include <optional>

namespace experiment {

/** @brief 决定启动阶段的下一档探测速率，并在真实身份核验后结束恢复。 */
class StartupBaudRecovery {
 public:
  StartupBaudRecovery(int configured_baud, bool has_persisted_cns_box);

  /** @brief 恢复结束后使用最新运行配置，避免在线 M1 换速后沿用启动旧值。 */
  int ProbeBaud(int current_baud) const;
  bool UseTrustedEndpoint() const;
  void ProbeFailed();
  void ProbeFound();
  void LinkLost();
  void IdentityVerified();
  void Cancel();
  bool Active() const;
  /** @brief 有待完成的启动速率复核时禁止 M1 探测抢占串口。 */
  bool AllowM1Probe() const;
  /** @brief 候选基准链路复核身份前不得发送周期帧。 */
  bool AllowPeriodicOutput() const;
  std::optional<int> RecoveredBaud() const;

 private:
  int configured_baud_;
  bool fallback_enabled_{};
  bool active_{true};
  bool use_baseline_{};
  bool link_found_{};
  std::optional<int> recovered_baud_;
};

}  // namespace experiment
