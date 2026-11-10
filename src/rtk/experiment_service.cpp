/** @file experiment_service.cpp @brief F407箱内基站与PX4附属观测服务。 */
#include "rtk/experiment_service.hpp"

#include <deque>
#include <optional>
#include <span>

#include "config/app_config.hpp"
#include "logging/logger.hpp"
#include "payload/json_serializer.hpp"
#include "rtk/base_station.hpp"
#include "rtk/px4_session.hpp"
#include "rtk/rtcm_encoder.hpp"
#include "uart/mavlink_link.hpp"

namespace rtk {
namespace {
using Time = ExperimentService::Clock::time_point;
using Json = nlohmann::json;
using namespace std::chrono_literals;

std::string Hex(std::span<const std::uint8_t> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result;
}

std::string_view RtcmPurpose(std::uint16_t type) {
  switch (type) {
    case 1005: return "reference_station_coordinates";
    case 1077: return "GPS_MSM7";
    case 1087: return "GLONASS_MSM7";
    case 1097: return "Galileo_MSM7";
    case 1127: return "BeiDou_MSM7";
    case 1230: return "GLONASS_code_phase_biases";
    default: return "unclassified";
  }
}

Json FrameSample(const RtcmFrame& frame, Time now) {
  return {{"frame_sequence", frame.sequence}, {"message_type", frame.message_type},
          {"purpose", RtcmPurpose(frame.message_type)}, {"length_bytes", frame.bytes.size()},
          {"crc_valid", true}, {"raw_hex", Hex(frame.bytes)},
          {"age_ms", std::chrono::duration_cast<std::chrono::milliseconds>(now - frame.received_at).count()}};
}
}  // namespace

struct ExperimentService::Impl {
  Impl(const config::RtkBaseConfig& value, logging::Logger& log)
      : config(value), logger(log),
        base({value.device, value.baud, value.survey_min_duration_s,
              value.survey_accuracy_m, value.stale_after_ms}) {}

  void Tick(Time now);
  void Disconnect(Time now);
  Json Status(Time now, bool include_samples) const;
  std::expected<uart::SentFrame, uart::UartError> Send(mavlink_message_t message);
  void Relay(Time now);
  std::string RelayState(Time now, const Json& base_status) const;
  bool Ready(Time now) const;

  config::RtkBaseConfig config;
  logging::Logger& logger;
  BaseStation base;
  Px4Session px4;
  std::optional<uart::MavlinkLink> link;
  Time next_open{};
  bool serial_failure_logged{false};
  bool identity_was_verified{false};
  std::string relay_mode{"stopped"};
  std::uint8_t correction_sequence{0};
  mavlink_status_t tx_status{};
  std::uint64_t run_sequence{}, received_frames{}, forwarded_frames{}, sent_fragments{},
      sent_payload_bytes{}, sent_wire_bytes{}, dropped_inactive{}, dropped_unready{},
      dropped_stale{}, dropped_overflow{}, send_failures{}, samples_replaced{};
  std::optional<Time> run_started, run_stopped, last_sent, last_eligible_rtcm;
  std::deque<RtcmFrame> pending;
  std::optional<RtcmFrame> latest_rtcm;
  Json latest_transfer = nullptr;
  std::optional<Time> transfer_started;
  std::string relay_error;
};

void ExperimentService::Impl::Tick(Time now) {
  const bool ready_before = Ready(now);
  base.Tick(now);
  for (auto& frame : base.DrainRtcmFrames()) {
    ++received_frames;
    if (latest_rtcm) ++samples_replaced;
    latest_rtcm = frame;
    if (relay_mode != "running" || !frame.eligible_for_relay) {
      ++dropped_inactive;
      continue;
    }
    last_eligible_rtcm = frame.received_at;
    if (pending.size() == 32) {
      pending.pop_front();
      ++dropped_overflow;
    }
    pending.push_back(std::move(frame));
  }
  if (!link && now >= next_open) {
    next_open = now + 1s;
    auto opened = uart::MavlinkLink::Open(config.px4_device, config.px4_baud);
    if (opened) {
      link.emplace(std::move(*opened));
      serial_failure_logged = false;
      logger.Info("RTK附属PX4串口已打开，等待观测身份核验");
    } else if (!serial_failure_logged) {
      logger.Warn("RTK附属PX4串口不可用，箱子与基站实验继续运行");
      serial_failure_logged = true;
    }
  }
  if (link) {
    for (unsigned frame = 0; frame < 64; ++frame) {
      auto received = link->ReceiveFrame(0ms);
      if (!received) {
        Disconnect(now);
        break;
      }
      if (!*received) break;
      (void)px4.Observe((*received)->message, now, (*received)->bytes);
    }
  }
  px4.Tick(now);
  if (link) {
    for (const auto& message : px4.PollOutbound(now)) {
      if (!Send(message)) {
        Disconnect(now);
        break;
      }
    }
  }
  if (!ready_before && Ready(now)) {
    dropped_unready += pending.size();
    pending.clear();
    last_eligible_rtcm.reset();
    base.DiscardRtcmBeforeActivation(now);
  }
  Relay(now);
  const bool verified = px4.Online(now);
  if (verified != identity_was_verified) {
    logger.Info(verified ? "RTK附属PX4观测身份已核验：" + *px4.Identity()
                         : "RTK附属PX4实时观测失效，箱子身份与基站实验继续保持");
    identity_was_verified = verified;
  }
}

std::expected<uart::SentFrame, uart::UartError> ExperimentService::Impl::Send(
    mavlink_message_t message) {
  // 两个协议生产者共用一个线上序号，不能沿用各自 pack() 的独立通道状态。
  const auto* entry = mavlink_get_msg_entry(message.msgid);
  mavlink_finalize_message_buffer(&message, message.sysid, message.compid, &tx_status,
      entry->min_msg_len, message.len, entry->crc_extra);
  return link->SendFrame(message);
}

std::string ExperimentService::Impl::RelayState(Time at, const Json& base_status) const {
  if (relay_mode != "running") return relay_mode;
  if (!base_status.at("receiver_verified").get<bool>() ||
      !base_status.at("survey_in").at("completed").get<bool>()) return "waiting_base";
  if (!link || !px4.Online(at)) return "waiting_px4";
  if (base_status.at("rtcm").at("activation_barrier").get<bool>() ||
      !last_eligible_rtcm || at - *last_eligible_rtcm > 1s) return "waiting_rtcm";
  return "forwarding";
}

bool ExperimentService::Impl::Ready(Time at) const {
  const auto status = base.Status(at);
  return link && px4.Online(at) && status.at("receiver_verified").get<bool>() &&
         status.at("survey_in").at("completed").get<bool>();
}

void ExperimentService::Impl::Relay(Time at) {
  while (!pending.empty() && at - pending.front().received_at > 1s) {
    pending.pop_front();
    ++dropped_stale;
  }
  if (RelayState(at, base.Status(at)) != "forwarding") {
    dropped_unready += pending.size();
    pending.clear();
    return;
  }
  if (pending.empty()) return;
  // 一轮最多一个完整RTCM帧，组内片连续发送，下一轮先处理遥测和心跳。
  auto frame = std::move(pending.front());
  pending.pop_front();
  const auto messages = EncodeRtcm(frame.bytes, correction_sequence);
  latest_transfer = {{"run_sequence", run_sequence},
                     {"rtcm", FrameSample(frame, at)}, {"fragments", Json::array()},
                     {"complete", false}, {"evidence", "local_serial_write"}};
  transfer_started = at;
  for (const auto& message : messages) {
    const auto sent = Send(message);
    if (!sent) {
      ++send_failures;
      relay_error = "correction_serial_write_failed";
      Disconnect(at);
      return;
    }
    mavlink_gps_rtcm_data_t data{};
    mavlink_msg_gps_rtcm_data_decode(&message, &data);
    ++sent_fragments;
    sent_payload_bytes += data.len;
    sent_wire_bytes += sent->bytes.size();
    latest_transfer["fragments"].push_back({
        {"message_id", MAVLINK_MSG_ID_GPS_RTCM_DATA}, {"flags", data.flags},
        {"fragmented", (data.flags & 1) != 0}, {"fragment_id", (data.flags >> 1) & 3},
        {"sequence_id", data.flags >> 3}, {"length_bytes", data.len},
        {"payload_hex", Hex(std::span(data.data).first(data.len))},
        {"wire_hex", Hex(sent->bytes)}, {"wire_length_bytes", sent->bytes.size()},
        {"write_succeeded", true}});
  }
  ++forwarded_frames;
  last_sent = at;
  relay_error.clear();
  latest_transfer["complete"] = true;
}

void ExperimentService::Impl::Disconnect(Time now) {
  link.reset();
  px4.Invalidate();
  dropped_unready += pending.size();
  pending.clear();
  next_open = now + 1s;
  if (!serial_failure_logged) {
    logger.Warn("RTK附属PX4串口断开，已清除其瞬时观测");
    serial_failure_logged = true;
  }
}

Json ExperimentService::Impl::Status(Time now, bool include_samples) const {
  const bool verified = px4.Online(now);
  const auto identity = verified ? px4.Identity() : std::nullopt;
  const auto endpoint = px4.Endpoint();
  const auto age = px4.LastFrameAge(now);
  auto gps = px4.GpsObservation(now, run_started, run_stopped);
  gps["transition_history_scope"] = "current_run";
  if (!run_started) gps["transitions"] = Json::array();
  for (auto& transition : gps["transitions"]) {
    transition["run_sequence"] = run_sequence;
    transition["run_elapsed_ms"] = std::chrono::duration_cast<
        std::chrono::milliseconds>(now - *run_started).count() -
        transition.at("age_ms").get<std::int64_t>();
  }
  auto telemetry = verified ? payload::ToRealtimeJson(px4.Snapshot(), 0).at("telemetry")
                            : Json::object();
  if (gps.at("stale").get<bool>()) telemetry.erase("gps");
  auto base_status = base.Status(now);
  const auto relay_state = RelayState(now, base_status);
  if (!include_samples) {
    base_status["survey_in"].erase("raw_hex");
    gps.erase("raw_hex");
    gps.erase("transitions");
  }
  Json observation{{"capture_mode", "bounded_latest"}, {"complete_capture", false},
      {"samples_included", include_samples}, {"rtcm_sample_capacity", 1},
      {"transfer_sample_capacity", 1}, {"rtcm_samples_replaced", samples_replaced}};
  if (include_samples) {
    observation["latest_rtcm"] = latest_rtcm ? FrameSample(*latest_rtcm, now) : Json(nullptr);
    observation["latest_transfer"] = latest_transfer;
    if (transfer_started)
      observation["latest_transfer"]["age_ms"] =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - *transfer_started).count();
  }
  return {
      {"base", std::move(base_status)},
      {"px4", {{"device", config.px4_device},
               {"baud", config.px4_baud},
               {"serial_connected", link.has_value()},
               {"identity_verified", verified},
               {"device_id", identity ? Json(*identity) : Json(nullptr)},
               {"frame_age_ms", age ? Json(age->count()) : Json(nullptr)},
               {"system_id", endpoint ? Json(endpoint->system_id) : Json(nullptr)},
               {"component_id", endpoint ? Json(endpoint->component_id) : Json(nullptr)},
               {"telemetry", std::move(telemetry)},
               {"gps_observation", std::move(gps)}, {"firmware", px4.FirmwareObservation()}}},
      {"relay", {{"state", relay_state}, {"requested", relay_mode == "running"},
                 {"run_sequence", run_sequence}, {"received_frames", received_frames},
                 {"forwarded_frames", forwarded_frames}, {"sent_fragments", sent_fragments},
                 {"sent_payload_bytes", sent_payload_bytes}, {"sent_wire_bytes", sent_wire_bytes},
                 {"pending_frames", pending.size()}, {"queue_capacity", 32},
                 {"max_frame_age_ms", 1000}, {"dropped_inactive", dropped_inactive},
                 {"dropped_unready", dropped_unready}, {"dropped_stale", dropped_stale},
                 {"dropped_overflow", dropped_overflow}, {"send_failures", send_failures},
                 {"delivery_evidence", "local_serial_write_only"},
                 {"receiver_ack_available", false},
                 {"run_elapsed_ms", run_started ? Json(std::chrono::duration_cast<
                     std::chrono::milliseconds>(run_stopped.value_or(now) - *run_started).count()) : Json(nullptr)},
                 {"last_sent_age_ms", last_sent ? Json(std::chrono::duration_cast<
                     std::chrono::milliseconds>(now - *last_sent).count()) : Json(nullptr)},
                 {"last_error", relay_error.empty() ? Json(nullptr) : Json(relay_error)}}},
      {"observation", std::move(observation)},
  };
}

ExperimentService::ExperimentService(const config::RtkBaseConfig& config,
                                       logging::Logger& logger)
    : impl_(std::make_unique<Impl>(config, logger)) {}
ExperimentService::~ExperimentService() = default;
void ExperimentService::Tick(Clock::time_point now) { impl_->Tick(now); }
nlohmann::json ExperimentService::BaseStatus(Clock::time_point now) const {
  return impl_->base.Status(now);
}
nlohmann::json ExperimentService::Status(Clock::time_point now, bool include_samples) const {
  return impl_->Status(now, include_samples);
}
std::expected<void, std::string> ExperimentService::StartSurveyIn(
    std::uint32_t duration_s, double accuracy_m) {
  const auto result = impl_->base.StartSurveyIn(duration_s, accuracy_m);
  if (result && impl_->relay_mode == "running") {
    impl_->relay_mode = "paused";
    impl_->dropped_inactive += impl_->pending.size();
    impl_->pending.clear();
  }
  return result;
}

std::expected<void, std::string> ExperimentService::ControlRelay(std::string_view operation, Time now) {
  auto& value = *impl_;
  if (operation == "rtk_rover_start") {
    if (value.relay_mode != "stopped") return std::unexpected("请先停止当前移动站实验");
    ++value.run_sequence;
    value.run_started = now;
    value.run_stopped.reset();
    value.relay_mode = "running";
  } else if (operation == "rtk_rover_resume") {
    if (value.relay_mode != "paused") return std::unexpected("当前实验未暂停");
    value.relay_mode = "running";
  } else if (operation == "rtk_rover_pause") {
    if (value.relay_mode == "stopped") return std::unexpected("移动站实验尚未开始");
    value.relay_mode = "paused";
  } else if (operation == "rtk_rover_stop") {
    if (value.run_started && !value.run_stopped) value.run_stopped = now;
    value.relay_mode = "stopped";
  } else {
    return std::unexpected("未知移动站操作");
  }
  value.dropped_inactive += value.pending.size();
  value.pending.clear();
  value.last_eligible_rtcm.reset();
  if (value.relay_mode == "running") value.base.DiscardRtcmBeforeActivation(now);
  return {};
}
void ExperimentService::CancelPendingConfiguration() {
  impl_->base.CancelPendingConfiguration();
}
}  // namespace rtk
