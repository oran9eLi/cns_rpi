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
