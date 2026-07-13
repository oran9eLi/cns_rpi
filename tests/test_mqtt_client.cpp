#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "mqtt/mqtt_client.hpp"

TEST_CASE("入站队列先进先出且容量固定为64") {
  mqtt::IncomingMessageQueue queue;
  for (int i = 0; i < 64; ++i) {
    CHECK(queue.Push({"topic", std::to_string(i)}));
  }
  CHECK_FALSE(queue.Push({"topic", "overflow"}));
  for (int i = 0; i < 64; ++i) {
    auto message = queue.TryPop();
    REQUIRE(message.has_value());
    CHECK(message->topic == "topic");
    CHECK(message->payload == std::to_string(i));
  }
  CHECK_FALSE(queue.TryPop().has_value());
}

TEST_CASE("非法Will topic导致客户端创建失败") {
  auto client = mqtt::MqttClient::Open({
      .broker_host = "127.0.0.1",
      .broker_port = 1,
      .client_id = "registration-test",
      .username = "",
      .password = "",
      .keepalive_seconds = 60,
      .will = {.topic = "invalid/#", .payload = "{}", .qos = 2, .retain = true},
      .subscriptions = {},
  });
  CHECK_FALSE(client.has_value());
}

TEST_CASE("连接broker后订阅消息进入主线程队列") {
  const char* port_text = std::getenv("CNS_TEST_MQTT_BROKER_PORT");
  if (!port_text) {
    MESSAGE("未设置CNS_TEST_MQTT_BROKER_PORT，跳过真实broker集成检查");
    return;
  }

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string topic = "cns_rpi/test/" + std::to_string(stamp);
  auto client = mqtt::MqttClient::Open({
      .broker_host = "127.0.0.1",
      .broker_port = std::stoi(port_text),
      .client_id = "cns-rpi-test-" + std::to_string(stamp),
      .username = "",
      .password = "",
      .keepalive_seconds = 10,
      .reconnect_delay_seconds = 1,
      .reconnect_delay_max_seconds = 2,
      .will = {.topic = topic + "/will", .payload = "{}", .qos = 0, .retain = false},
      .subscriptions = {{topic, 1}},
  });
  REQUIRE(client.has_value());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!client->IsConnected() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(client->IsConnected());
  for (int i = 0; i < 20; ++i) {
    REQUIRE(client->Publish(topic, "plain-" + std::to_string(i), 1, false));
  }
  REQUIRE(client->PublishAndWait(topic, "payload", 1, false,
                                 std::chrono::seconds(2)));

  std::optional<mqtt::IncomingMessage> received;
  while (!received && std::chrono::steady_clock::now() < deadline) {
    auto candidate = client->TryPopMessage();
    if (candidate && candidate->payload == "payload") received = std::move(candidate);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(received.has_value());
  CHECK(received->topic == topic);
  CHECK(received->payload == "payload");
}
