/**
 * @file mqtt_client.cpp
 * @brief mqtt_client.hpp 的实现。
 */

#include "mqtt/mqtt_client.hpp"

#include <iostream>
#include <mutex>
#include <utility>

#include <mosquitto.h>

namespace mqtt {

namespace {

void OnConnect(struct mosquitto* /*mosq*/, void* userdata, int rc) {
  auto* connected = static_cast<std::atomic<bool>*>(userdata);
  if (rc == 0) {
    connected->store(true);
    std::cout << "MQTT已连接" << std::endl;
  } else {
    std::cerr << "MQTT连接失败: rc=" << rc << std::endl;
  }
}

void OnDisconnect(struct mosquitto* /*mosq*/, void* userdata, int /*rc*/) {
  auto* connected = static_cast<std::atomic<bool>*>(userdata);
  connected->store(false);
  std::cout << "MQTT连接断开" << std::endl;
}

}  // namespace

MqttClient::MqttClient(mosquitto* handle, std::unique_ptr<std::atomic<bool>> connected)
    : handle_(handle), connected_(std::move(connected)) {}

MqttClient::MqttClient(MqttClient&& other) noexcept
    : handle_(other.handle_), connected_(std::move(other.connected_)) {
  other.handle_ = nullptr;
}

MqttClient& MqttClient::operator=(MqttClient&& other) noexcept {
  if (this != &other) {
    if (handle_) {
      mosquitto_loop_stop(handle_, /*force=*/true);
      mosquitto_destroy(handle_);
    }
    handle_ = other.handle_;
    connected_ = std::move(other.connected_);
    other.handle_ = nullptr;
  }
  return *this;
}

MqttClient::~MqttClient() {
  if (handle_) {
    mosquitto_loop_stop(handle_, /*force=*/true);
    mosquitto_destroy(handle_);
  }
}

std::optional<MqttClient> MqttClient::Open(const ConnectionOptions& options) {
  static std::once_flag lib_init_flag;
  std::call_once(lib_init_flag, [] { mosquitto_lib_init(); });

  auto connected = std::make_unique<std::atomic<bool>>(false);
  mosquitto* handle =
      mosquitto_new(options.client_id.c_str(), /*clean_session=*/true, connected.get());
  if (!handle) {
    return std::nullopt;
  }

  if (!options.username.empty()) {
    if (mosquitto_username_pw_set(handle, options.username.c_str(), options.password.c_str()) !=
        MOSQ_ERR_SUCCESS) {
      mosquitto_destroy(handle);
      return std::nullopt;
    }
  }

  mosquitto_connect_callback_set(handle, &OnConnect);
  mosquitto_disconnect_callback_set(handle, &OnDisconnect);
  if (mosquitto_reconnect_delay_set(handle, /*reconnect_delay=*/1, /*reconnect_delay_max=*/30,
                                     /*reconnect_exponential_backoff=*/true) != MOSQ_ERR_SUCCESS) {
    mosquitto_destroy(handle);
    return std::nullopt;
  }

  if (mosquitto_connect_async(handle, options.broker_host.c_str(), options.broker_port,
                               options.keepalive_seconds) != MOSQ_ERR_SUCCESS) {
    mosquitto_destroy(handle);
    return std::nullopt;
  }

  if (mosquitto_loop_start(handle) != MOSQ_ERR_SUCCESS) {
    mosquitto_destroy(handle);
    return std::nullopt;
  }

  return MqttClient(handle, std::move(connected));
}

bool MqttClient::Publish(const std::string& topic, const std::string& payload, int qos,
                          bool retain) {
  int rc = mosquitto_publish(handle_, /*mid=*/nullptr, topic.c_str(),
                              static_cast<int>(payload.size()), payload.data(), qos, retain);
  return rc == MOSQ_ERR_SUCCESS;
}

bool MqttClient::IsConnected() const { return connected_->load(); }

}  // namespace mqtt
