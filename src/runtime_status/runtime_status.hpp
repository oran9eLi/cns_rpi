#pragma once

/**
 * @file runtime_status.hpp
 * @brief 主控箱运行三态的协议模型、静默计时和事件驱动发布判定。
 *
 * @details
 * 本模块只实现新版设备接入基线中的固定五字段协议，不读取串口、不建立 MQTT
 * 连接，也不修改 registration。调用方提供真实身份和有效业务帧事实，并把生成的
 * 发布物交给 MQTT 边界发送。
 */

#include <chrono>
#include <expected>
#include <optional>
#include <string>

namespace runtime_status {

using Clock = std::chrono::steady_clock;

enum class ControlStatus { kOnline, kOffline };
enum class BusinessStatus { kOnline, kOffline, kUnknown };
enum class IdentityStatus { kVerified, kCached, kConflict, kUnbound };

/// 一条完整运行三态；device_id 必须与 MQTT Topic 中的设备段一致。
struct Snapshot {
  std::string device_id;
  ControlStatus control_status{ControlStatus::kOnline};
  BusinessStatus business_status{BusinessStatus::kUnknown};
  IdentityStatus identity_status{IdentityStatus::kUnbound};

  bool operator==(const Snapshot&) const = default;
};

/// 可直接交给 MQTT 客户端的完整发布参数。
struct Publication {
  std::string topic;
  std::string payload;
  int qos{1};
  bool retain{true};
};

/**
 * @brief 校验快照并生成固定 schema v1 发布物。
 * @return device_id 或状态组合非法时返回中文错误，不生成可发布载荷。
 */
std::expected<Publication, std::string> BuildPublication(
    const std::string& topic_namespace, const std::string& topic_suffix,
    const Snapshot& snapshot);

/**
 * @brief 生成 MQTT Last Will。
 * @details 离线遗嘱只表达 Pi 控制链离线，业务固定 unknown；身份只能根据是否有
 * 合法持久化绑定选择 cached 或 unbound。
 */
std::expected<Publication, std::string> BuildLastWillPublication(
    const std::string& topic_namespace, const std::string& topic_suffix,
    const std::string& device_id, bool has_persisted_binding);

/**
 * @brief 跟踪一台主控箱本次进程内的业务和身份事实。
 * @details 控制链状态由发布时的 MQTT 连接事实决定，因此普通快照恒为 online；
 * offline 只能由 BuildLastWillPublication 构造，避免生成非法离线组合。
 */
class Tracker {
 public:
  Tracker(std::string device_id,
          std::optional<std::string> persisted_device_id,
          Clock::time_point started_at);

  /// 记录一条由现有解码器确认的 F407 有效业务帧。
  void ObserveBusinessFrame(Clock::time_point now);

  /// 记录从真实 OPEN_DRONE_ID_BASIC_ID 提取并校验通过的当前身份。
  void ObserveIdentity(const std::string& current_device_id);

  /// 推进固定 10 秒静默判定；调用频率不会改变状态语义。
  void Tick(Clock::time_point now);

  Snapshot CurrentOnlineSnapshot() const;

 private:
  std::string device_id_;
  std::optional<std::string> persisted_device_id_;
  Clock::time_point started_at_;
  std::optional<Clock::time_point> last_business_frame_;
  BusinessStatus business_status_{BusinessStatus::kUnknown};
  IdentityStatus identity_status_{IdentityStatus::kUnbound};
};

/// 只在内容变化或 MQTT 连接上升沿请求发布；失败时调用方不得 MarkPublished。
class PublicationState {
 public:
  bool ShouldPublish(bool mqtt_connected, const Snapshot& current);
  void MarkPublished(const Snapshot& snapshot);

 private:
  bool was_connected_{false};
  bool connection_requires_publish_{false};
  std::optional<Snapshot> last_published_;
};

}  // namespace runtime_status
