#pragma once

/**
 * @file pi_link_inspection.hpp
 * @brief 只读 Pi 控制链自检的校验、事实快照和进程内终态去重。
 *
 * @details 不持有 MQTT 或串口，不发 MAVLink 帧。调用方仅传入已持久化的设备绑定，
 * 并在首次合法请求时按需提供当前进程事实；重投不得重新采样。
 */

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "device/device_binding_store.hpp"
#include "runtime_status/runtime_status.hpp"

namespace experiment {

/// 串口值只表示本进程是否持有打开的端点；空值表示无法可靠判断。
struct Observation {
  runtime_status::Snapshot runtime;
  std::optional<bool> serial_port_open;
  bool mqtt_connected{false};
};

/// 主线程未持有串口时，发现线程仍可能持有候选端点，故返回未知而非误报关闭。
std::optional<bool> SerialEndpointFact(bool active_link_open);

/// 设备侧终态 MQTT 发布物，固定 QoS 2 且不保留。
struct Publication {
  std::string topic;
  std::string payload;
  int qos{2};
  bool retain{false};
};

struct Outcome {
  std::optional<Publication> publication;
  std::string diagnostic;
};

/// 仅对已持久化的主控箱绑定生成 QoS 2 订阅；PX4 和无绑定设备不认领实验 Topic。
std::optional<std::pair<std::string, int>> DeviceSubscription(
    const std::string& topic_namespace,
    const std::optional<device::Binding>& persisted_binding);

/**
 * @brief 处理固定只读动作并按 action_id 保存有界终态。
 * @param topic_namespace 已校验的 MQTT 命名空间。
 * @details 同内容重投复用首次终态事实与观测时间，只替换合法的 command_id；
 * 缓存至少保留 300 秒、最多 128 项。仅在主业务线程调用。
 */
class PiLinkInspector {
 public:
  explicit PiLinkInspector(std::string topic_namespace);

  using WallClock = std::chrono::system_clock;
  using SteadyClock = std::chrono::steady_clock;
  using Sampler = std::function<std::optional<Observation>()>;

  /// 严格校验设备下发载荷；无法安全关联命令时不生成猜测性 ACK。
  Outcome Handle(std::string_view topic, std::string_view payload,
                 const std::optional<device::Binding>& persisted_binding,
                 const Sampler& sample, WallClock::time_point wall_now,
                 SteadyClock::time_point steady_now);

  /// 当前保留的终态动作数，供缓存上界测试与运行诊断使用。
  std::size_t CachedActionCount() const;

 private:
  struct Entry {
    std::string action_id;
    std::string comparison;
    std::string terminal_payload;
    SteadyClock::time_point terminated_at;
  };
  std::string topic_namespace_;
  std::deque<Entry> entries_;
};

/**
 * @brief 有界保存尚未得到 MQTT 完成回调的原始 ACK。
 * @details 发布失败或断线后只重发保存的原文，不能重新调用事实采样器。
 */
class AckOutbox {
 public:
  static constexpr std::size_t kCapacity = 128;

  /// 满容量时返回 false，原有待发 ACK 不被覆盖。
  bool Enqueue(Publication publication);

  const Publication* Front() const;
  void ConfirmFront();

  /// 一次最多尝试一条；发布失败保留原文，成功后移除。
  bool FlushOne(const std::function<bool(const Publication&)>& publish);
  std::size_t Size() const;

 private:
  std::deque<Publication> pending_;
};

/// 历史终态仅在待发队列空闲时一次补入一条，避免挤掉实时只读自检 ACK。
bool FeedRecoveredAckWhenIdle(AckOutbox& outbox,
                              std::deque<Publication>& recovered);

}  // namespace experiment
