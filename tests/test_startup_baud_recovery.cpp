#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <utility>

#include "experiment/startup_baud_recovery.hpp"

TEST_CASE("重启后先按持久化速率探测，失联后才尝试基准速率") {
  experiment::StartupBaudRecovery recovery(57600, true);
  CHECK(recovery.ProbeBaud(57600) == 57600);
  CHECK_FALSE(recovery.UseTrustedEndpoint());

  recovery.ProbeFailed();
  CHECK(recovery.ProbeBaud(57600) == 115200);
  CHECK(recovery.UseTrustedEndpoint());

  recovery.ProbeFailed();
  CHECK(recovery.ProbeBaud(57600) == 57600);
}

TEST_CASE("仅在基准速率上重新核验身份后请求持久化恢复值") {
  experiment::StartupBaudRecovery recovery(57600, true);
  recovery.ProbeFailed();
  recovery.ProbeFound();
  CHECK_FALSE(recovery.RecoveredBaud().has_value());

  recovery.IdentityVerified();
  CHECK(recovery.RecoveredBaud() == 115200);
  CHECK_FALSE(recovery.Active());
}

TEST_CASE("已保存速率可用时不产生恢复写入") {
  experiment::StartupBaudRecovery recovery(57600, true);
  recovery.ProbeFound();
  recovery.IdentityVerified();
  CHECK_FALSE(recovery.RecoveredBaud().has_value());
  CHECK_FALSE(recovery.Active());
}

TEST_CASE("没有已绑定主控箱或原本就是基准速率时不尝试第二速率") {
  for (const auto& [rate, binding] : {
           std::pair{57600, false}, std::pair{115200, true}}) {
    experiment::StartupBaudRecovery recovery(rate, binding);
    recovery.ProbeFailed();
    CHECK(recovery.ProbeBaud(rate) == rate);
    CHECK_FALSE(recovery.UseTrustedEndpoint());
  }
}

TEST_CASE("运行中实验动作禁止启动恢复探测") {
  experiment::StartupBaudRecovery recovery(57600, true);
  CHECK_FALSE(recovery.AllowM1Probe());
  recovery.Cancel();
  recovery.ProbeFailed();
  CHECK(recovery.ProbeBaud(115200) == 115200);
  CHECK(recovery.AllowM1Probe());
  CHECK_FALSE(recovery.Active());
}

TEST_CASE("身份复核之前链路中断不改配置并继续尝试两档速率") {
  experiment::StartupBaudRecovery recovery(57600, true);
  recovery.ProbeFailed();
  recovery.ProbeFound();
  recovery.LinkLost();
  CHECK_FALSE(recovery.RecoveredBaud().has_value());
  CHECK(recovery.Active());
  CHECK(recovery.ProbeBaud(57600) == 57600);
}

TEST_CASE("基准速率候选链路在身份复核前保持被动监听") {
  experiment::StartupBaudRecovery recovery(57600, true);
  CHECK_FALSE(recovery.AllowM1Probe());
  CHECK(recovery.AllowPeriodicOutput());
  recovery.ProbeFailed();
  CHECK_FALSE(recovery.AllowM1Probe());
  recovery.ProbeFound();
  CHECK_FALSE(recovery.AllowM1Probe());
  CHECK_FALSE(recovery.AllowPeriodicOutput());
  recovery.IdentityVerified();
  CHECK(recovery.AllowM1Probe());
  CHECK(recovery.AllowPeriodicOutput());
}

TEST_CASE("无需启动回退的链路不改变M1探测能力") {
  experiment::StartupBaudRecovery recovery(115200, true);
  CHECK(recovery.AllowM1Probe());
}
