#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "mqtt/topic.hpp"

TEST_CASE("BuildTelemetryTopic按{topic_prefix}/{vendor_id}/telemetry拼接") {
  CHECK(mqtt::BuildTelemetryTopic("cns_rpi", "DCDWCNS1ABCDEFGHIJKL") ==
        "cns_rpi/DCDWCNS1ABCDEFGHIJKL/telemetry");
}

TEST_CASE("BuildTelemetryTopic对空topic_prefix仍正常拼接(防御性场景，不做校验)") {
  CHECK(mqtt::BuildTelemetryTopic("", "DCDWCNS1ABCDEFGHIJKL") == "/DCDWCNS1ABCDEFGHIJKL/telemetry");
}

TEST_CASE("BuildTelemetryTopic对空vendor_id仍正常拼接(防御性场景，不做校验)") {
  CHECK(mqtt::BuildTelemetryTopic("cns_rpi", "") == "cns_rpi//telemetry");
}
