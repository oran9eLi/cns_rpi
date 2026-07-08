#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "payload/json_serializer.hpp"

TEST_CASE("空TelemetryState只输出identity.school_name其余顶层key都不存在") {
  state::TelemetryState state{};

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("identity"));
  CHECK(json["identity"]["school_name"] == "NNUTC");
  CHECK_FALSE(json["identity"].contains("vendor_id"));
  CHECK_FALSE(json["identity"].contains("dcdw_label"));
  CHECK_FALSE(json["identity"].contains("rpi_serial"));
  CHECK_FALSE(json.contains("telemetry"));
  CHECK_FALSE(json.contains("modules"));
  CHECK_FALSE(json.contains("alarms"));
  CHECK_FALSE(json.contains("logs"));
  CHECK_FALSE(json.contains("drone_id"));
}

TEST_CASE("identity三个可选字段各自独立按需省略") {
  state::TelemetryState state{};
  state.vendor_id = "DCDWCNS1ABCDEFGHIJKL";

  auto json = payload::ToJson(state, "NNUTC");

  CHECK(json["identity"]["vendor_id"] == "DCDWCNS1ABCDEFGHIJKL");
  CHECK_FALSE(json["identity"].contains("dcdw_label"));
  CHECK_FALSE(json["identity"].contains("rpi_serial"));

  state.dcdw_label = "DCDW-007";
  state.rpi_serial = "100000001234abcd";
  auto json2 = payload::ToJson(state, "NNUTC");

  CHECK(json2["identity"]["dcdw_label"] == "DCDW-007");
  CHECK(json2["identity"]["rpi_serial"] == "100000001234abcd");
}

TEST_CASE("heartbeat字段按原始数字透传,未收到时telemetry.heartbeat不存在") {
  state::TelemetryState state{};
  mavlink_heartbeat_t hb{};
  hb.custom_mode = 0;
  hb.type = 2;
  hb.autopilot = 12;
  hb.base_mode = 81;
  hb.system_status = 4;
  hb.mavlink_version = 3;
  state.heartbeat = hb;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("telemetry"));
  REQUIRE(json["telemetry"].contains("heartbeat"));
  CHECK(json["telemetry"]["heartbeat"]["type"] == 2);
  CHECK(json["telemetry"]["heartbeat"]["autopilot"] == 12);
  CHECK(json["telemetry"]["heartbeat"]["base_mode"] == 81);
  CHECK(json["telemetry"]["heartbeat"]["system_status"] == 4);
  CHECK(json["telemetry"]["heartbeat"]["mavlink_version"] == 3);
  CHECK_FALSE(json["telemetry"].contains("attitude"));
}

TEST_CASE("attitude弧度转角度") {
  state::TelemetryState state{};
  mavlink_attitude_t att{};
  att.time_boot_ms = 123456;
  att.roll = 1.0F;
  att.pitch = -0.5F;
  att.yaw = 0.0F;
  att.rollspeed = 0.1F;
  att.pitchspeed = -0.1F;
  att.yawspeed = 0.0F;
  state.attitude = att;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json["telemetry"].contains("attitude"));
  CHECK(json["telemetry"]["attitude"]["time_boot_ms"] == 123456);
  CHECK(json["telemetry"]["attitude"]["roll"].get<double>() == doctest::Approx(57.29578));
  CHECK(json["telemetry"]["attitude"]["pitch"].get<double>() == doctest::Approx(-28.64789));
}
