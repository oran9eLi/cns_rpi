#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <optional>

#include "runtime_status/device_session.hpp"

using namespace std::chrono_literals;

namespace {

constexpr auto kStartedAt = runtime_status::Clock::time_point{};

device::Binding CachedBinding() {
  return {
      .device_id = "DCDWCNS1GHC0G6LF8MY6",
      .device_type = device::Type::kCnsBox,
  };
}

}  // namespace

TEST_CASE("合法缓存可在尚未复核F407时恢复MQTT身份") {
  runtime_status::DeviceSession session(CachedBinding(), kStartedAt);

  REQUIRE(session.ActiveBinding() != nullptr);
  CHECK(*session.ActiveBinding() == CachedBinding());
  REQUIRE(session.CurrentOnlineStatus().has_value());
  CHECK(session.CurrentOnlineStatus()->identity_status ==
        runtime_status::IdentityStatus::kCached);
  CHECK(session.HasPersistedBinding());
  CHECK_FALSE(session.CanSendDeviceCommands());
}

TEST_CASE("无缓存时不生成身份而真实身份到达后保持未绑定") {
  runtime_status::DeviceSession session(std::nullopt, kStartedAt);
  CHECK(session.ActiveBinding() == nullptr);
  CHECK_FALSE(session.CurrentOnlineStatus().has_value());

  const auto result = session.ObserveIdentity(CachedBinding());
  CHECK(result == runtime_status::IdentityObservation::kUnbound);
  REQUIRE(session.ActiveBinding() != nullptr);
  CHECK(session.ActiveBinding()->device_id == "DCDWCNS1GHC0G6LF8MY6");
  REQUIRE(session.CurrentOnlineStatus().has_value());
  CHECK(session.CurrentOnlineStatus()->identity_status ==
        runtime_status::IdentityStatus::kUnbound);
  CHECK_FALSE(session.HasPersistedBinding());
}

TEST_CASE("首次身份持久化成功后转为已验证") {
  runtime_status::DeviceSession session(std::nullopt, kStartedAt);
  REQUIRE(session.ObserveIdentity(CachedBinding()) ==
          runtime_status::IdentityObservation::kUnbound);

  session.ConfirmPersistedBinding();

  CHECK(session.HasPersistedBinding());
  CHECK(session.CurrentOnlineStatus()->identity_status ==
        runtime_status::IdentityStatus::kVerified);
}

TEST_CASE("串口断开只撤销设备动作和遥测资格不删除绑定") {
  runtime_status::DeviceSession session(CachedBinding(), kStartedAt);
  session.SetLinkAvailable(true);
  REQUIRE(session.ObserveIdentity(CachedBinding()) ==
          runtime_status::IdentityObservation::kVerified);
  session.ObserveBusinessFrame(kStartedAt + 1s);
  CHECK(session.CanSendDeviceCommands());
  CHECK(session.CanPublishTelemetry());

  session.SetLinkAvailable(false);

  CHECK(session.HasPersistedBinding());
  REQUIRE(session.ActiveBinding() != nullptr);
  CHECK_FALSE(session.CanSendDeviceCommands());
  CHECK_FALSE(session.CanPublishTelemetry());
  CHECK(session.CurrentOnlineStatus()->identity_status ==
        runtime_status::IdentityStatus::kVerified);
}

TEST_CASE("冲突身份不切换绑定并阻断动作和遥测") {
  runtime_status::DeviceSession session(CachedBinding(), kStartedAt);
  session.SetLinkAvailable(true);
  auto conflict = CachedBinding();
  conflict.device_id = "DCDWCNS1OTHERDEVICE";

  const auto result = session.ObserveIdentity(conflict);

  CHECK(result == runtime_status::IdentityObservation::kConflict);
  REQUIRE(session.ActiveBinding() != nullptr);
  CHECK(session.ActiveBinding()->device_id == "DCDWCNS1GHC0G6LF8MY6");
  CHECK(session.CurrentOnlineStatus()->identity_status ==
        runtime_status::IdentityStatus::kConflict);
  CHECK_FALSE(session.CanSendDeviceCommands());
  CHECK_FALSE(session.CanPublishTelemetry());
}

TEST_CASE("业务静默满十秒只改变业务状态且MQTT身份仍保留") {
  runtime_status::DeviceSession session(CachedBinding(), kStartedAt);
  session.SetLinkAvailable(true);
  REQUIRE(session.ObserveIdentity(CachedBinding()) ==
          runtime_status::IdentityObservation::kVerified);
  session.ObserveBusinessFrame(kStartedAt + 1s);

  session.Tick(kStartedAt + 11s);

  REQUIRE(session.CurrentOnlineStatus().has_value());
  CHECK(session.CurrentOnlineStatus()->business_status ==
        runtime_status::BusinessStatus::kOffline);
  CHECK(session.CurrentOnlineStatus()->identity_status ==
        runtime_status::IdentityStatus::kVerified);
  CHECK(session.ActiveBinding() != nullptr);
}
