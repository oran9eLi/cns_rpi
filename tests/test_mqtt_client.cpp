#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "mqtt/mqtt_client.hpp"

TEST_CASE("非法Will topic导致客户端创建失败") {
  auto client = mqtt::MqttClient::Open({
      .broker_host = "127.0.0.1",
      .broker_port = 1,
      .client_id = "registration-test",
      .username = "",
      .password = "",
      .keepalive_seconds = 60,
      .will = {.topic = "invalid/#", .payload = "{}", .qos = 2, .retain = true},
  });
  CHECK_FALSE(client.has_value());
}
