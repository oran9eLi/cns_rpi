#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <unistd.h>

#include "logging/logger.hpp"
#include "mqtt/mqtt_client.hpp"

namespace {

class BlockingStringBuffer : public std::stringbuf {
 public:
  ~BlockingStringBuffer() override { Release(); }

  bool WaitUntilWriteBegins(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return write_begun_; });
  }

  void Release() {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

 protected:
  std::streamsize xsputn(const char* text, std::streamsize count) override {
    {
      std::unique_lock lock(mutex_);
      write_begun_ = true;
      condition_.notify_all();
      condition_.wait(lock, [this] { return released_; });
    }
    return std::stringbuf::xsputn(text, count);
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool write_begun_ = false;
  bool released_ = false;
};

std::unique_ptr<logging::Logger> MakeCaptureLogger(std::ostringstream& out,
                                                   std::ostringstream& err) {
  auto logger = logging::Logger::Create(
      {.minimum_level = logging::Level::kDebug, .file = {}, .max_file_size_bytes = 0}, out, err);
  REQUIRE(logger.has_value());
  return std::move(*logger);
}

}  // namespace

TEST_CASE("Broker重启后自动恢复实验Topic的QoS2订阅") {
  const char* port_text = std::getenv("CNS_TEST_MQTT_RECONNECT_PORT");
  if (!port_text) {
    MESSAGE("未设置CNS_TEST_MQTT_RECONNECT_PORT，跳过本地Broker重启检查");
    return;
  }
  const int port = std::stoi(port_text);

  std::ostringstream log_out;
  std::ostringstream log_err;
  auto logger = MakeCaptureLogger(log_out, log_err);
  const std::string topic = "cns_rpi/RECONNECTTEST/experiment/set";
  auto client = mqtt::MqttClient::Open({
      .broker_host = "127.0.0.1",
      .broker_port = port,
      .client_id = "cns-rpi-experiment-reconnect-" + std::to_string(::getpid()),
      .username = "",
      .password = "",
      .keepalive_seconds = 10,
      .reconnect_delay_seconds = 1,
      .reconnect_delay_max_seconds = 1,
      .will = {.topic = topic + "/will", .payload = "{}", .qos = 0,
               .retain = false},
      .subscriptions = {{topic, 2}},
  }, *logger);
  INFO(log_err.str());
  REQUIRE(client.has_value());
  auto wait_connected = [&](std::uint64_t later_than) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
      if (client->IsConnected() && client->ConnectionGeneration() > later_than) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  };
  auto publish_and_receive = [&](const std::string& payload) {
    CHECK(client->PublishAndWait(topic, payload, 2, false, std::chrono::seconds(2)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      if (auto message = client->TryPopMessage(); message && message->payload == payload) {
        CHECK(message->topic == topic);
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  };
  REQUIRE(wait_connected(0));
  REQUIRE(publish_and_receive("首次连接"));
  std::cerr << "本地Broker首次收发成功；现在停止Broker。\n";
  const auto first_generation = client->ConnectionGeneration();
  const auto disconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (client->IsConnected() && std::chrono::steady_clock::now() < disconnect_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE_FALSE(client->IsConnected());
  std::cerr << "MQTT断线已检测；现在重启Broker。\n";
  REQUIRE(wait_connected(first_generation));
  CHECK(publish_and_receive("重连后"));
}

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

TEST_CASE("MQTT遗嘱在发起Broker连接前完成配置") {
  const auto source_path =
      std::filesystem::path(SOURCE_DIR) / "src/mqtt/mqtt_client.cpp";
  std::ifstream input(source_path);
  REQUIRE(input.is_open());
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};

  const auto will_position = source.find("mosquitto_will_set(");
  const auto connect_position = source.find("mosquitto_connect_async(");
  REQUIRE(will_position != std::string::npos);
  REQUIRE(connect_position != std::string::npos);
  CHECK(will_position < connect_position);
}

TEST_CASE("非法Will topic导致客户端创建失败") {
  std::ostringstream log_out;
  std::ostringstream log_err;
  auto logger = MakeCaptureLogger(log_out, log_err);
  auto client = mqtt::MqttClient::Open({
      .broker_host = "127.0.0.1",
      .broker_port = 1,
      .client_id = "registration-test",
      .username = "",
      .password = "",
      .keepalive_seconds = 60,
      .will = {.topic = "invalid/#", .payload = "{}", .qos = 2, .retain = true},
      .subscriptions = {},
  }, *logger);
  CHECK_FALSE(client.has_value());
  CHECK(log_err.str().find("MQTT遗嘱设置失败") != std::string::npos);
}

TEST_CASE("连接broker后订阅消息进入主线程队列") {
  const char* port_text = std::getenv("CNS_TEST_MQTT_BROKER_PORT");
  if (!port_text) {
    MESSAGE("未设置CNS_TEST_MQTT_BROKER_PORT，跳过真实broker集成检查");
    return;
  }

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string topic = "cns_rpi/test/" + std::to_string(stamp);
  BlockingStringBuffer log_buffer;
  std::ostream log_out(&log_buffer);
  std::ostringstream log_err;
  auto logger_result = logging::Logger::Create(
      {.minimum_level = logging::Level::kDebug, .file = {}, .max_file_size_bytes = 0}, log_out,
      log_err);
  REQUIRE(logger_result.has_value());
  auto logger = std::move(*logger_result);
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
  }, *logger);
  REQUIRE(client.has_value());

  const bool log_write_begun = log_buffer.WaitUntilWriteBegins(std::chrono::seconds(2));
  CHECK(log_write_begun);
  if (log_write_begun) CHECK_FALSE(client->IsConnected());
  log_buffer.Release();
  REQUIRE(log_write_begun);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!client->IsConnected() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(client->IsConnected());
  CHECK(log_buffer.str().find("MQTT已连接") != std::string::npos);
  for (int i = 0; i < 20; ++i) {
    REQUIRE(client->Publish(topic, "plain-" + std::to_string(i), 1, false));
  }
  REQUIRE(client->PublishAndWait(topic, "payload", 1, false,
                                 std::chrono::seconds(2)));

  auto tracked_mid = client->PublishTracked(topic, "实验ACK", 2, false);
  REQUIRE(tracked_mid.has_value());
  const auto tracked_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!client->TakePublishCompletion(*tracked_mid) &&
         std::chrono::steady_clock::now() < tracked_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(std::chrono::steady_clock::now() < tracked_deadline);
  CHECK_FALSE(client->TakePublishCompletion(*tracked_mid));

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
