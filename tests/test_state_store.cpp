#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "state/state_store.hpp"

TEST_CASE("初始状态所有字段都是nullopt") {
  state::StateStore store;

  auto snapshot = store.Snapshot();

  CHECK_FALSE(snapshot.heartbeat.has_value());
  CHECK_FALSE(snapshot.gps_raw_int.has_value());
  CHECK_FALSE(snapshot.attitude.has_value());
  CHECK_FALSE(snapshot.global_position_int.has_value());
  CHECK_FALSE(snapshot.sys_status.has_value());
  CHECK_FALSE(snapshot.battery_status.has_value());
  CHECK_FALSE(snapshot.scaled_pressure.has_value());
}

TEST_CASE("Update一个字段只影响那一个字段，其余仍是nullopt") {
  state::StateStore store;
  mavlink_heartbeat_t heartbeat{};
  heartbeat.type = 18;
  heartbeat.autopilot = 8;

  store.UpdateHeartbeat(heartbeat);
  auto snapshot = store.Snapshot();

  REQUIRE(snapshot.heartbeat.has_value());
  CHECK(snapshot.heartbeat->type == 18);
  CHECK(snapshot.heartbeat->autopilot == 8);
  CHECK_FALSE(snapshot.gps_raw_int.has_value());
  CHECK_FALSE(snapshot.attitude.has_value());
}

TEST_CASE("同一字段多次Update，Snapshot返回最新值") {
  state::StateStore store;
  mavlink_scaled_pressure_t first{};
  first.press_abs = 1000.0F;
  mavlink_scaled_pressure_t second{};
  second.press_abs = 1013.25F;

  store.UpdateScaledPressure(first);
  store.UpdateScaledPressure(second);
  auto snapshot = store.Snapshot();

  REQUIRE(snapshot.scaled_pressure.has_value());
  CHECK(snapshot.scaled_pressure->press_abs == doctest::Approx(1013.25F));
}
