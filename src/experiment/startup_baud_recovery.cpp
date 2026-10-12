/**
 * @file startup_baud_recovery.cpp
 * @brief 启动阶段串口速率恢复状态实现。
 */

#include "experiment/startup_baud_recovery.hpp"

namespace experiment {

StartupBaudRecovery::StartupBaudRecovery(int configured_baud,
                                         bool has_persisted_cns_box)
    : configured_baud_(configured_baud),
      fallback_enabled_(has_persisted_cns_box && configured_baud == 57600) {}

int StartupBaudRecovery::ProbeBaud(int current_baud) const {
  if (!active_) return current_baud;
  return use_baseline_ ? 115200 : configured_baud_;
}

bool StartupBaudRecovery::UseTrustedEndpoint() const {
  return active_ && use_baseline_;
}

void StartupBaudRecovery::ProbeFailed() {
  if (active_ && fallback_enabled_ && !link_found_) {
    use_baseline_ = !use_baseline_;
  }
}

void StartupBaudRecovery::ProbeFound() {
  if (active_) link_found_ = true;
}

void StartupBaudRecovery::LinkLost() {
  if (active_ && link_found_) {
    link_found_ = false;
    ProbeFailed();
  }
}

void StartupBaudRecovery::IdentityVerified() {
  if (!active_ || !link_found_) return;
  if (use_baseline_) recovered_baud_ = 115200;
  active_ = false;
}

void StartupBaudRecovery::Cancel() {
  active_ = false;
  use_baseline_ = false;
}

bool StartupBaudRecovery::Active() const { return active_; }
bool StartupBaudRecovery::AllowM1Probe() const {
  return !(active_ && fallback_enabled_);
}
bool StartupBaudRecovery::AllowPeriodicOutput() const {
  return !(active_ && use_baseline_ && link_found_);
}

std::optional<int> StartupBaudRecovery::RecoveredBaud() const {
  return recovered_baud_;
}

}  // namespace experiment
