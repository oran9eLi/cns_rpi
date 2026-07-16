#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "mqtt/topic.hpp"

TEST_CASE("按namespace/vendor_id/suffix构造设备topic") {
  CHECK(mqtt::BuildRegistrationTopic("cns_rpi", "ABC123", "registration") ==
        "cns_rpi/ABC123/registration");
  CHECK(mqtt::BuildTelemetryTopic("cns_rpi", "ABC123", "telemetry") ==
        "cns_rpi/ABC123/telemetry");
}

TEST_CASE("topic构造函数只拼接不重复校验") {
  CHECK(mqtt::BuildRegistrationTopic("", "ABC123", "registration") ==
        "/ABC123/registration");
  CHECK(mqtt::BuildTelemetryTopic("cns_rpi", "", "telemetry") == "cns_rpi//telemetry");
}

TEST_CASE("构造配置命令ACK和设备来源请求topic") {
  CHECK(mqtt::BuildConfigSetTopic("cns_rpi", "ABC123", "config/set") ==
        "cns_rpi/ABC123/config/set");
  CHECK(mqtt::BuildConfigAckTopic("cns_rpi", "ABC123", "config/ack") ==
        "cns_rpi/ABC123/config/ack");
  CHECK(mqtt::BuildControlSetTopic("cns_rpi", "ABC123", "control/set") ==
        "cns_rpi/ABC123/control/set");
  CHECK(mqtt::BuildControlAckTopic("cns_rpi", "ABC123", "control/ack") ==
        "cns_rpi/ABC123/control/ack");
  CHECK(mqtt::BuildConfigRequestTopic("cns_rpi", "ABC123") ==
        "cns_rpi/sources/ABC123/config/request");
}
