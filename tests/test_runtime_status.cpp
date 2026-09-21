#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "runtime_status/runtime_status.hpp"

using namespace std::chrono_literals;

namespace {

constexpr auto kStartedAt = runtime_status::Clock::time_point{};

runtime_status::Snapshot VerifiedOnline() {
  return {
      .device_id = "DCDWCNS1GHC0G6LF8MY6",
      .control_status = runtime_status::ControlStatus::kOnline,
      .business_status = runtime_status::BusinessStatus::kOnline,
      .identity_status = runtime_status::IdentityStatus::kVerified,
  };
}

}  // namespace

TEST_CASE("运行三态发布物严格使用约定Topic、QoS和retained") {
  const auto publication = runtime_status::BuildPublication(
      "cns_rpi", "runtime/status/v1", VerifiedOnline());

  REQUIRE(publication.has_value());
  CHECK(publication->topic ==
        "cns_rpi/DCDWCNS1GHC0G6LF8MY6/runtime/status/v1");
  CHECK(publication->qos == 1);
  CHECK(publication->retain);
}

TEST_CASE("完整在线三态JSON只包含契约规定的五个字段") {
  const auto publication = runtime_status::BuildPublication(
      "cns_rpi", "runtime/status/v1", VerifiedOnline());
  REQUIRE(publication.has_value());
  const auto payload = nlohmann::json::parse(publication->payload);

  CHECK(payload == nlohmann::json{
                       {"schema_version", 1},
                       {"device_id", "DCDWCNS1GHC0G6LF8MY6"},
                       {"control_status", "online"},
                       {"business_status", "online"},
                       {"identity_status", "verified"},
                   });
  CHECK_FALSE(payload.contains("vendor_id"));
}

TEST_CASE("有缓存和无缓存分别生成两种合法离线遗嘱") {
  const auto cached = runtime_status::BuildLastWillPublication(
      "cns_rpi", "runtime/status/v1", "DCDWCNS1GHC0G6LF8MY6",
      /*has_persisted_binding=*/true);
  const auto unbound = runtime_status::BuildLastWillPublication(
      "cns_rpi", "runtime/status/v1", "DCDWCNS1GHC0G6LF8MY6",
      /*has_persisted_binding=*/false);

  REQUIRE(cached.has_value());
  REQUIRE(unbound.has_value());
  CHECK(nlohmann::json::parse(cached->payload) == nlohmann::json{
            {"schema_version", 1},
            {"device_id", "DCDWCNS1GHC0G6LF8MY6"},
            {"control_status", "offline"},
            {"business_status", "unknown"},
            {"identity_status", "cached"},
        });
  CHECK(nlohmann::json::parse(unbound->payload) == nlohmann::json{
            {"schema_version", 1},
            {"device_id", "DCDWCNS1GHC0G6LF8MY6"},
            {"control_status", "offline"},
            {"business_status", "unknown"},
            {"identity_status", "unbound"},
        });
}

TEST_CASE("控制链离线时拒绝业务在线或实时身份结论") {
  for (const auto business : {
           runtime_status::BusinessStatus::kOnline,
           runtime_status::BusinessStatus::kOffline,
       }) {
    auto invalid = VerifiedOnline();
    invalid.control_status = runtime_status::ControlStatus::kOffline;
    invalid.business_status = business;
    CHECK_FALSE(runtime_status::BuildPublication(
                    "cns_rpi", "runtime/status/v1", invalid)
                    .has_value());
  }

  for (const auto identity : {
           runtime_status::IdentityStatus::kVerified,
           runtime_status::IdentityStatus::kConflict,
       }) {
    auto invalid = VerifiedOnline();
    invalid.control_status = runtime_status::ControlStatus::kOffline;
    invalid.business_status = runtime_status::BusinessStatus::kUnknown;
    invalid.identity_status = identity;
    CHECK_FALSE(runtime_status::BuildPublication(
                    "cns_rpi", "runtime/status/v1", invalid)
                    .has_value());
  }
}

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

TEST_CASE("状态未变化时不周期发布而重连必须重新发布") {
  runtime_status::PublicationState state;
  const auto snapshot = VerifiedOnline();

  CHECK_FALSE(state.ShouldPublish(/*mqtt_connected=*/false, snapshot));
  CHECK(state.ShouldPublish(/*mqtt_connected=*/true, snapshot));
  state.MarkPublished(snapshot);
  CHECK_FALSE(state.ShouldPublish(/*mqtt_connected=*/true, snapshot));
  CHECK_FALSE(state.ShouldPublish(/*mqtt_connected=*/false, snapshot));
  CHECK(state.ShouldPublish(/*mqtt_connected=*/true, snapshot));
}

TEST_CASE("发布失败时保持待发布状态") {
  runtime_status::PublicationState state;
  const auto snapshot = VerifiedOnline();

  CHECK(state.ShouldPublish(/*mqtt_connected=*/true, snapshot));
  CHECK(state.ShouldPublish(/*mqtt_connected=*/true, snapshot));
}
