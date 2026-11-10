#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <string>

#include "runtime_status/runtime_status.hpp"

using namespace std::chrono_literals;

namespace {

constexpr auto kStartedAt = runtime_status::Clock::time_point{};

}  // namespace

TEST_CASE("有效业务帧持续到达时保持在线且静默满十秒才离线") {
  runtime_status::Tracker tracker(
      "DCDWCNS1GHC0G6LF8MY6",
      std::optional<std::string>{"DCDWCNS1GHC0G6LF8MY6"}, kStartedAt);
  tracker.ObserveIdentity("DCDWCNS1GHC0G6LF8MY6");
  tracker.ObserveBusinessFrame(kStartedAt + 1s);
  tracker.ObserveBusinessFrame(kStartedAt + 9s);

  tracker.Tick(kStartedAt + 18s + 999ms);
  CHECK(tracker.CurrentOnlineSnapshot().business_status ==
        runtime_status::BusinessStatus::kOnline);

  tracker.Tick(kStartedAt + 19s);
  CHECK(tracker.CurrentOnlineSnapshot().business_status ==
        runtime_status::BusinessStatus::kOffline);
}

TEST_CASE("新有效业务帧使业务状态从离线恢复为在线") {
  runtime_status::Tracker tracker(
      "DCDWCNS1GHC0G6LF8MY6",
      std::optional<std::string>{"DCDWCNS1GHC0G6LF8MY6"}, kStartedAt);
  tracker.Tick(kStartedAt + 10s);
  REQUIRE(tracker.CurrentOnlineSnapshot().business_status ==
          runtime_status::BusinessStatus::kOffline);

  tracker.ObserveBusinessFrame(kStartedAt + 11s);
  CHECK(tracker.CurrentOnlineSnapshot().business_status ==
        runtime_status::BusinessStatus::kOnline);
}

TEST_CASE("身份一致、未复核、冲突和无绑定产生四种身份状态") {
  runtime_status::Tracker cached(
      "DCDWCNS1GHC0G6LF8MY6",
      std::optional<std::string>{"DCDWCNS1GHC0G6LF8MY6"}, kStartedAt);
  CHECK(cached.CurrentOnlineSnapshot().identity_status ==
        runtime_status::IdentityStatus::kCached);

  cached.ObserveIdentity("DCDWCNS1GHC0G6LF8MY6");
  CHECK(cached.CurrentOnlineSnapshot().identity_status ==
        runtime_status::IdentityStatus::kVerified);

  cached.ObserveIdentity("DCDWCNS1OTHERDEVICE");
  CHECK(cached.CurrentOnlineSnapshot().identity_status ==
        runtime_status::IdentityStatus::kConflict);

  runtime_status::Tracker unbound("DCDWCNS1GHC0G6LF8MY6", std::nullopt,
                                  kStartedAt);
  unbound.ObserveIdentity("DCDWCNS1GHC0G6LF8MY6");
  CHECK(unbound.CurrentOnlineSnapshot().identity_status ==
        runtime_status::IdentityStatus::kUnbound);
}

TEST_CASE("未绑定身份持久化后可转为已验证") {
  runtime_status::Tracker tracker("DCDWCNS1GHC0G6LF8MY6", std::nullopt,
                                  kStartedAt);
  tracker.ObserveIdentity("DCDWCNS1GHC0G6LF8MY6");
  REQUIRE(tracker.CurrentOnlineSnapshot().identity_status ==
          runtime_status::IdentityStatus::kUnbound);

  tracker.ConfirmPersistedIdentity("DCDWCNS1GHC0G6LF8MY6");

  CHECK(tracker.CurrentOnlineSnapshot().identity_status ==
        runtime_status::IdentityStatus::kVerified);
}
