#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "latency/px4_latency.hpp"

TEST_CASE("解析并原样确认PX4真实RTT探测") {
  const auto parsed = latency::ParsePx4LatencyProbe(
      R"({"schema_version":1,"device_id":"PX4U2-ABC123","session_id":"session_test","probe_id":"00000000-0000-4000-8000-000000000001"})",
      "PX4U2-ABC123");

  REQUIRE(parsed.has_value());
  const auto ack = latency::BuildPx4LatencyAck(*parsed);
  CHECK(ack["schema_version"] == 1);
  CHECK(ack["device_id"] == "PX4U2-ABC123");
  CHECK(ack["session_id"] == "session_test");
  CHECK(ack["probe_id"] == "00000000-0000-4000-8000-000000000001");
}

TEST_CASE("拒绝错误设备和非法关联标识符") {
  CHECK_FALSE(latency::ParsePx4LatencyProbe(
      R"({"schema_version":1,"device_id":"OTHER","session_id":"session_test","probe_id":"00000000-0000-4000-8000-000000000001"})",
      "PX4U2-ABC123"));
  CHECK_FALSE(latency::ParsePx4LatencyProbe(
      R"({"schema_version":1,"device_id":"PX4U2-ABC123","session_id":"bad/session","probe_id":"00000000-0000-4000-8000-000000000001"})",
      "PX4U2-ABC123"));
}
