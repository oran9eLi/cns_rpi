#pragma once

#include <functional>
#include <memory>

#include <nlohmann/json_fwd.hpp>

#include "experiment/pi_link_inspection.hpp"
#include "video/video_manager.hpp"

namespace experiment {

/// 固定视频动作的身份校验、摄像头会话归属和进程内去重；不持有 MQTT 连接。
class VideoExperiment {
 public:
  using TerminalObserver = std::function<void(const nlohmann::json&)>;
  /// 终态形成时调用观察者，不等待 MQTT ACK 队列腾出位置。
  VideoExperiment(std::string topic_namespace, bool enabled, video::Manager& manager,
                  TerminalObserver on_terminal = {});
  ~VideoExperiment();
  Outcome Handle(std::string_view topic, std::string_view payload,
                 const std::optional<device::Binding>& persisted_binding,
                 const PiLinkInspector::Sampler& sample,
                 PiLinkInspector::WallClock::time_point wall_now,
                 PiLinkInspector::SteadyClock::time_point steady_now);
  /// 即使 ACK 队列已满也推进子进程；终态保存到可以交付时再返回。
  std::optional<Publication> Tick(PiLinkInspector::WallClock::time_point wall_now,
                                PiLinkInspector::SteadyClock::time_point steady_now,
                                bool can_publish);
  bool Busy() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace experiment
