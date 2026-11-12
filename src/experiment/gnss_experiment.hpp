#pragma once

/** @file gnss_experiment.hpp @brief GNSS原帧采集、固定目录归档和有界MQTT批次；仅由主循环调用。 */

#include <chrono>
#include <deque>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
#include "config/app_config.hpp"
#include "experiment/gnss_request.hpp"
#include "experiment/pi_link_inspection.hpp"
#include "gnss/raw_frame.hpp"

namespace experiment {
/// 持久化身份负责路由，实时连接只约束新采集；类自身不持有串口或MQTT客户端。
class GnssExperiment {
 public:
  using Wall = std::chrono::system_clock;
  using Steady = std::chrono::steady_clock;
  GnssExperiment(std::string topic_namespace, config::GnssConfig config);
  /// 成功开始只确认采集已建立；同action重投复用原ACK，结束后需另行查询状态。
  Outcome Handle(std::string_view topic, std::string_view payload,
                 const std::optional<std::string>& identity, bool live,
                 Wall::time_point wall, Steady::time_point now);
  /// message已通过外层MAVLink校验；now必须保留串口实际接收时刻以隔离开始前排队片。
  void OnMessage(const mavlink_message_t& message, Wall::time_point wall, Steady::time_point now);
  void Tick(Wall::time_point wall, Steady::time_point now, bool live);
  void TransportLost(std::string_view reason, Wall::time_point wall);
  bool Busy() const { return active_; }
  /// 只在收到对应MQTT完成回调后ConfirmBatch；发布调用返回成功尚不移除原文。
  const Publication* FrontBatch() const;
  void ConfirmBatch();
  void SetBatchInterval(std::chrono::milliseconds interval) { batch_interval_ = interval; }
  nlohmann::json Status() const;

 private:
  struct Entry { GnssRequest request; nlohmann::json ack; Steady::time_point at; };
  std::string Start(const GnssRequest& request, Wall::time_point wall, Steady::time_point now);
  void Finish(std::string_view reason, bool failed, Wall::time_point wall);
  bool Write(const nlohmann::json& record);
  bool Flush(Wall::time_point wall);
  nlohmann::json Batch(Wall::time_point wall) const;
  std::string topic_namespace_;
  config::GnssConfig config_;
  gnss::Assembler assembler_;
  std::deque<Entry> entries_;
  std::deque<Publication> batches_;
  std::uint64_t highest_lease_{}, archive_bytes_{}, frames_{}, raw_bytes_{}, bad_checksum_{},
      batch_seq_{}, confirmed_batches_{};
  std::string lease_session_, device_id_, session_id_, capture_id_, state_{"idle"}, end_reason_;
  std::string write_error_;
  bool active_{false};
  Steady::time_point started_{}, deadline_{}, batch_started_{};
  std::chrono::milliseconds batch_interval_{200};
  nlohmann::json frames_batch_ = nlohmann::json::array();
  std::ofstream archive_;
};
}
