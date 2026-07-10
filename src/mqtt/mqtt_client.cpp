/**
 * @file mqtt_client.cpp
 * @brief mqtt_client.hpp 的实现。
 */

#include "mqtt/mqtt_client.hpp"

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <utility>

#include <mosquitto.h>

namespace mqtt {

struct ClientState {
  std::atomic<bool> connected{false};
  std::mutex publish_mutex;
  std::condition_variable publish_cv;
  int completed_mid = -1;
};

namespace {

void OnConnect(struct mosquitto* /*mosq*/, void* userdata, int rc) {
  auto* state = static_cast<ClientState*>(userdata);
  if (rc == 0) {
    state->connected.store(true);
    std::cout << "MQTT已连接" << std::endl;
  } else {
    std::cerr << "MQTT连接失败: rc=" << rc << std::endl;
  }
}

void OnDisconnect(struct mosquitto* /*mosq*/, void* userdata, int /*rc*/) {
  auto* state = static_cast<ClientState*>(userdata);
  state->connected.store(false);
  std::cout << "MQTT连接断开" << std::endl;
}

void OnPublish(struct mosquitto* /*mosq*/, void* userdata, int mid) {
  auto* state = static_cast<ClientState*>(userdata);
  {
    std::lock_guard lock(state->publish_mutex);
    state->completed_mid = mid;
  }
  state->publish_cv.notify_all();
}

}  // namespace

MqttClient::MqttClient(mosquitto* handle, std::unique_ptr<ClientState> state)
    : handle_(handle), state_(std::move(state)) {}

MqttClient::MqttClient(MqttClient&& other) noexcept
    : handle_(other.handle_), state_(std::move(other.state_)) {
  other.handle_ = nullptr;
}

MqttClient& MqttClient::operator=(MqttClient&& other) noexcept {
  if (this != &other) {
    if (handle_) {
      mosquitto_disconnect(handle_);
      mosquitto_loop_stop(handle_, /*force=*/true);
      mosquitto_destroy(handle_);
    }
    handle_ = other.handle_;
    state_ = std::move(other.state_);
    other.handle_ = nullptr;
  }
  return *this;
}

MqttClient::~MqttClient() {
  if (handle_) {
    mosquitto_disconnect(handle_);
    mosquitto_loop_stop(handle_, /*force=*/true);
    mosquitto_destroy(handle_);
  }
}

std::optional<MqttClient> MqttClient::Open(const ConnectionOptions& options) {
  static std::once_flag lib_init_flag;
  std::call_once(lib_init_flag, [] { mosquitto_lib_init(); });

  auto state = std::make_unique<ClientState>();
  mosquitto* handle = mosquitto_new(options.client_id.c_str(), /*clean_session=*/true, state.get());
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
  mosquitto_publish_callback_set(handle, &OnPublish);
  if (mosquitto_will_set(handle, options.will.topic.c_str(),
                         static_cast<int>(options.will.payload.size()),
                         options.will.payload.data(), options.will.qos,
                         options.will.retain) != MOSQ_ERR_SUCCESS) {
    mosquitto_destroy(handle);
    return std::nullopt;
  }
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

  return MqttClient(handle, std::move(state));
}

bool MqttClient::Publish(const std::string& topic, const std::string& payload, int qos,
                          bool retain) {
  int rc = mosquitto_publish(handle_, /*mid=*/nullptr, topic.c_str(),
                              static_cast<int>(payload.size()), payload.data(), qos, retain);
  return rc == MOSQ_ERR_SUCCESS;
}

bool MqttClient::PublishAndWait(const std::string& topic, const std::string& payload, int qos,
                                bool retain, std::chrono::milliseconds timeout) {
  std::unique_lock lock(state_->publish_mutex);
  state_->completed_mid = -1;
  int mid = -1;
  const int rc = mosquitto_publish(handle_, &mid, topic.c_str(), static_cast<int>(payload.size()),
                                   payload.data(), qos, retain);
  if (rc != MOSQ_ERR_SUCCESS) {
    return false;
  }
  return state_->publish_cv.wait_for(lock, timeout,
                                     [this, mid] { return state_->completed_mid == mid; });
}

bool MqttClient::IsConnected() const { return state_->connected.load(); }

}  // namespace mqtt
