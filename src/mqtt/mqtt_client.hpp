#pragma once

/**
 * @file mqtt_client.hpp
 * @brief 包一层 libmosquitto：连接/自动重连/发布，不涉及 topic 命名、不涉及
 * 何时连接/何时发布的业务判断（那是 main.cpp 的事，见
 * docs/superpowers/specs/2026-07-09-m5-mqtt-publish-design.md §6）。
 *
 * @details
 * V1 只发布，不订阅——命令下行/ACK 回传是 M6 的事，这里不暴露任何订阅接口。
 * 断线重连交给 libmosquitto 自己的后台线程（mosquitto_loop_start +
 * mosquitto_reconnect_delay_set），这个类不实现应用层重试逻辑。
 * 用前向声明 struct mosquitto，不在头文件里包含 <mosquitto.h>——这样只
 * 声明/调用这个类接口的调用方（main.cpp）不需要 libmosquitto 的公开头文件
 * 也能编译，只有 mqtt_client.cpp 本身需要真正链接 libmosquitto。
 * 依赖边界：只依赖标准库，不包含 state/、payload/、config/ 等其他模块头文件
 * （ConnectionOptions 的字段是从 config::MqttConfig 摘出来的原始值，不是
 * 直接依赖 config::MqttConfig 这个类型，调用方在 main.cpp 里做字段搬运）。
 */

#include <chrono>
#include <memory>
#include <optional>
#include <string>

struct mosquitto;

namespace mqtt {

struct ClientState;

struct WillOptions {
  std::string topic;
  std::string payload;
  int qos = 2;
  bool retain = true;
};

/// 连接一个 MQTT broker 所需的全部参数，字段跟 config::MqttConfig 一一对应
/// （去掉了 mqtt_client.hpp 用不到的 topic_prefix/qos——那两个是 main.cpp
/// 拼 topic/传 Publish 参数时用的，不是"建连接"本身需要的）。
struct ConnectionOptions {
  std::string broker_host;
  int broker_port = 0;
  std::string client_id;
  std::string username;
  std::string password;
  int keepalive_seconds = 0;
  int reconnect_delay_seconds = 1;
  int reconnect_delay_max_seconds = 30;
  WillOptions will;
};

/// libmosquitto 客户端的 RAII 包装：一个实例对应一条到 broker 的连接。
class MqttClient {
 public:
  /**
   * @brief 创建客户端、发起异步连接、启动后台网络线程。
   * @details 连接是否已经建立成功不由这个函数保证——mosquitto_connect_async
   * 只是发起连接，真正连上是异步的，之后要用 IsConnected() 轮询。
   * @return 客户端对象创建/连接发起失败（mosquitto_new/mosquitto_connect_async/
   * mosquitto_loop_start 任一步返回错误）时返回 std::nullopt；否则返回一个还在
   * "连接中"或已经连上的客户端（具体状态由 IsConnected() 反映）。
   */
  static std::optional<MqttClient> Open(const ConnectionOptions& options);

  ~MqttClient();
  MqttClient(MqttClient&&) noexcept;
  MqttClient& operator=(MqttClient&&) noexcept;
  MqttClient(const MqttClient&) = delete;
  MqttClient& operator=(const MqttClient&) = delete;

  /**
   * @brief 发布一条消息。
   * @details 很薄的一层转发，不判断当前是否已连接（调用方应先用 IsConnected()
   * 把关，未连接时调用本函数本身不会崩溃，只会返回 false）。
   * @param topic 完整 topic 字符串（调用方用 mqtt::BuildTelemetryTopic() 拼好传入）。
   * @param payload 消息体，原样按字节发送（本项目里恒为 JSON 文本）。
   * @param qos 服务质量等级（0/1/2），来自 config.json 的 mqtt.qos。
   * @param retain 是否让 broker 保留这条消息给之后的新订阅者。
   * @return 发布调用成功返回 true，libmosquitto 返回非 MOSQ_ERR_SUCCESS 则 false。
   */
  bool Publish(const std::string& topic, const std::string& payload, int qos, bool retain);

  /// 发布并等待 libmosquitto 的完成回调，超时或发布失败返回 false。
  bool PublishAndWait(const std::string& topic, const std::string& payload, int qos, bool retain,
                      std::chrono::milliseconds timeout);

  /// 读取当前连接状态（由 on_connect/on_disconnect 回调维护，不主动问库）。
  bool IsConnected() const;

 private:
  MqttClient(mosquitto* handle, std::unique_ptr<ClientState> state);

  mosquitto* handle_;
  std::unique_ptr<ClientState> state_;
};

}  // namespace mqtt
