#pragma once

/**
 * @file publisher.hpp
 * @brief 对称遥测通道的无队列定时发布调度器。
 *
 * @details 调度器只读取一份最新状态快照并调用注入的发布函数，不依赖 MQTT
 * 客户端。失败帧不会进入重试队列，下一周期继续采样最新状态。
 */

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "state/state_store.hpp"

namespace telemetry {

using Clock = std::chrono::steady_clock;
using FrameBuilder = std::function<nlohmann::json(
    const state::TelemetryState&, std::uint64_t)>;
using SnapshotProvider = std::function<state::TelemetryState()>;
using PublishFunction = std::function<bool(
    const std::string&, const std::string&, int, bool)>;

struct ChannelOptions {
  std::string name;
  std::string topic;
  std::chrono::milliseconds interval;
  int qos{0};
  bool retain{false};
  bool enabled{true};
};

struct ChannelDefinition {
  ChannelOptions options;
  FrameBuilder build_frame;
};

struct PublishAttempt {
  std::string channel_name;
  bool succeeded{false};
  bool should_log_failure{false};
};

class Publisher {
 public:
  Publisher(std::vector<ChannelDefinition> channels,
            Clock::time_point started_at);

  std::vector<PublishAttempt> Tick(
      Clock::time_point now, const SnapshotProvider& snapshot_provider,
      const PublishFunction& publish);
  void Reset(Clock::time_point now);

 private:
  struct ChannelState {
    ChannelDefinition definition;
    Clock::time_point last_attempt;
    std::uint64_t sequence{0};
    bool failure_active{false};
  };

  std::vector<ChannelState> channels_;
};

}  // namespace telemetry
