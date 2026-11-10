/**
 * @file rtk_base_experiment.hpp
 * @brief RTK 基站动作与真实配置回执；不持有串口或 MQTT。
 */
#pragma once

#include <deque>
#include <expected>
#include <functional>
#include <optional>

#include <nlohmann/json.hpp>

#include "experiment/pi_link_inspection.hpp"
#include "experiment/rtk_base_request.hpp"

namespace experiment {

/// 仅由 RTK 主循环调用。接收机配置接受与配置读回完成是两个不同阶段。
class RtkBaseExperiment {
 public:
  using WallClock = std::chrono::system_clock;
  using Sampler = std::function<nlohmann::json()>;
  using Starter = std::function<std::expected<void, std::string>(std::uint32_t, double)>;
  using Canceller = std::function<void()>;
  using Operator = std::function<std::expected<void, std::string>(std::string_view)>;

  explicit RtkBaseExperiment(std::string topic_namespace);
  Outcome Handle(std::string_view topic, std::string_view payload,
                 const std::optional<std::string>& verified_identity,
                 const Sampler& sample, const Starter& start,
                 WallClock::time_point now, const Operator& operate = {},
                 const Sampler& rover_sample = {});
  /// 读回完成/失败/过期后只产生一次终态，交由已有 ACK 队列负责发送。
  std::optional<Publication> Tick(const Sampler& sample, const Canceller& cancel,
                                WallClock::time_point now);

 private:
  struct Entry {
    RtkBaseRequest request;
    nlohmann::json acknowledgement;
    bool pending{false};
    WallClock::time_point completed_at{};
  };
  Publication Publish(const Entry& entry) const;
  std::string topic_namespace_;
  std::deque<Entry> entries_;
  std::uint64_t highest_lease_{0};
  std::string lease_session_;
};

}  // namespace experiment
