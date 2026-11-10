/** @file gnss_experiment.cpp @brief 原帧只在采集中消费；归档和MQTT队列均有明确上限。 */
#include "experiment/gnss_experiment.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <span>

namespace experiment {
namespace {
using Json = nlohmann::json;
using Wall = GnssExperiment::Wall;
constexpr std::uint64_t kArchiveLimit = 16 * 1024 * 1024;
constexpr std::uint64_t kSummaryReserve = 4096;
constexpr std::size_t kBatchBytes = 16384;
constexpr std::size_t kBatchFrames = 32;
constexpr std::size_t kQueueCapacity = 8;
constexpr auto kBatchWait = std::chrono::milliseconds{200};
constexpr auto kActionRetention = std::chrono::seconds{300};

std::string Utc(Wall::time_point now) {
  const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
  const auto epoch = Wall::to_time_t(seconds);
  std::tm utc{};
  gmtime_r(&epoch, &utc);
  char date[32]{}, fraction[8]{};
  std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
  std::snprintf(fraction, sizeof(fraction), ".%03dZ", static_cast<int>(ms));
  return std::string(date) + fraction;
}

std::string Base64(std::span<const std::uint8_t> bytes) {
  constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve((bytes.size() + 2) / 3 * 4);
  for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
    const auto remaining = bytes.size() - offset;
    const auto value = (static_cast<std::uint32_t>(bytes[offset]) << 16) |
        (remaining > 1 ? static_cast<std::uint32_t>(bytes[offset + 1]) << 8 : 0) |
        (remaining > 2 ? bytes[offset + 2] : 0);
    result.push_back(alphabet[(value >> 18) & 63]);
    result.push_back(alphabet[(value >> 12) & 63]);
    result.push_back(remaining > 1 ? alphabet[(value >> 6) & 63] : '=');
    result.push_back(remaining > 2 ? alphabet[value & 63] : '=');
  }
  return result;
}

bool ArchivedCapture(const std::filesystem::path& path, const std::string& capture_id) {
  std::ifstream in(path);
  std::string line;
  if (!std::getline(in, line)) return false;
  const auto metadata = Json::parse(line, nullptr, false);
  return metadata.is_object() && metadata.contains("capture_id") &&
         metadata.at("capture_id") == capture_id;
}

Json Ack(const GnssRequest& request, Wall::time_point wall, const Json& status,
         std::string_view error = {}) {
  Json ack{{"schema_version", 1}, {"command_id", request.command_id},
      {"session_id", request.session_id}, {"action_id", request.action_id},
      {"operation", request.operation}, {"observed_at", Utc(wall)},
      {"status", error.empty() ? "completed" : "rejected"}, {"fact", {{"gnss", status}}}};
  if (!error.empty()) ack["error_code"] = error;
  return ack;
}
}  // namespace

GnssExperiment::GnssExperiment(std::string topic_namespace, config::GnssConfig config)
    : topic_namespace_(std::move(topic_namespace)), config_(std::move(config)) {}

Outcome GnssExperiment::Handle(std::string_view topic, std::string_view payload,
    const std::optional<std::string>& identity, bool live,
    Wall::time_point wall, Steady::time_point now) {
  const auto request = ParseGnssRequest(payload);
  if (!request) return {.publication = {}, .diagnostic = request.error()};
  if (!identity || topic != topic_namespace_ + "/" + *identity + "/experiment/set")
    return {.publication = {}, .diagnostic = "GNSS请求没有已绑定箱子身份或Topic不匹配"};
  const auto publish = [&](Json ack) -> Outcome {
    return {.publication = Publication{topic_namespace_ + "/" + *identity + "/experiment/ack",
        ack.dump(), 2, false}, .diagnostic = {}};
  };
  const auto reject = [&](std::string_view error) {
    return publish(Ack(*request, wall, Status(), error));
  };
  if (request->device_id != *identity) return reject("wrong_target");
  std::erase_if(entries_, [&](const auto& entry) { return now - entry.at >= kActionRetention; });
  const auto previous = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
    return entry.request.action_id == request->action_id;
  });
  if (previous != entries_.end()) {
    if (previous->request.comparison != request->comparison) return reject("action_conflict");
    auto ack = previous->ack;
    ack["command_id"] = request->command_id;
    return publish(std::move(ack));
  }
  if (request->expiry <= wall) return reject("expired");
  if (request->lease_version < highest_lease_ ||
      (request->lease_version == highest_lease_ && lease_session_ != request->session_id))
    return reject("stale_lease");
  if (entries_.size() >= 128) return reject("action_cache_full");
  if (request->operation == "gnss_capture_start") {
    if (!config_.enabled) return reject("gnss_disabled");
    if (!live) return reject("device_not_live");
    if (active_) return reject("capture_busy");
    if (!batches_.empty()) return reject("delivery_pending");
    const auto error = Start(*request, wall, now);
    if (!error.empty()) return reject(error);
  } else if (request->operation == "gnss_capture_stop") {
    if (capture_id_.empty()) return reject("capture_not_found");
    if (request->session_id != session_id_) return reject("capture_owner_mismatch");
    if (request->capture_id != capture_id_) return reject("capture_mismatch");
    if (active_) Finish("capture_stopped", false, wall);
  }
  highest_lease_ = request->lease_version;
  lease_session_ = request->session_id;
  auto ack = Ack(*request, wall, Status());
  entries_.push_back(Entry{*request, ack, now});
  return publish(std::move(ack));
}

std::string GnssExperiment::Start(const GnssRequest& request,
                                Wall::time_point wall, Steady::time_point now) {
  const auto current = config_.archive_directory / "current.jsonl";
  const auto previous = config_.archive_directory / "previous.jsonl";
  if (request.capture_id == capture_id_ || ArchivedCapture(current, request.capture_id) ||
      ArchivedCapture(previous, request.capture_id)) return "capture_conflict";
  std::error_code error;
  std::filesystem::create_directories(config_.archive_directory, error);
  if (error) return "archive_open_failed";
  if (std::filesystem::exists(current, error)) {
    std::filesystem::remove(previous, error);
    if (error) return "archive_open_failed";
    std::filesystem::rename(current, previous, error);
    if (error) return "archive_open_failed";
  }
  if (error) return "archive_open_failed";
  archive_.clear();
  archive_.open(current, std::ios::out | std::ios::trunc);
  if (!archive_.is_open()) return "archive_open_failed";
  device_id_ = request.device_id;
  session_id_ = request.session_id;
  capture_id_ = request.capture_id;
  state_ = "capturing";
  end_reason_.clear();
  write_error_.clear();
  archive_bytes_ = frames_ = raw_bytes_ = bad_checksum_ = batch_seq_ = confirmed_batches_ = 0;
  frames_batch_ = Json::array();
  assembler_.Reset();
  started_ = now;
  deadline_ = now + std::chrono::seconds{request.duration_seconds};
  active_ = true;
  if (!Write(Json{{"kind", "metadata"}, {"schema_version", 1}, {"device_id", device_id_},
      {"session_id", session_id_}, {"capture_id", capture_id_}, {"observed_at", Utc(wall)},
      {"duration_seconds", request.duration_seconds}})) {
    const auto reason = write_error_;
    Finish(reason, true, wall);
    return reason;
  }
  return {};
}

void GnssExperiment::OnMessage(const mavlink_message_t& message,
                               Wall::time_point wall, Steady::time_point now) {
  if (!active_) return;
  // 串口解析器可能在开始动作前已经排队，不能将这些旧片归入新一轮。
  if (now < started_) return;
  Tick(wall, now, true);
  if (!active_) return;
  const auto frame = assembler_.Add(message, now);
  if (!frame) return;
  Json record{{"record_seq", frames_ + 1},
      {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count()},
      {"received_at", Utc(wall)},
      {"source", {{"system_id", frame->system_id}, {"component_id", frame->component_id},
          {"frame_id", frame->frame_id}, {"received_uptime_ms", frame->received_uptime_ms}}},
      {"protocol", frame->protocol == 1 ? "nmea" : "ubx"},
      {"nmea_type", frame->nmea_type}, {"ubx_class", frame->ubx_class}, {"ubx_id", frame->ubx_id},
      {"source_checksum_valid", (frame->flags & 1) != 0}, {"wire_crc16", frame->wire_crc16},
      {"raw_base64", Base64(frame->wire)}, {"raw_length", frame->wire.size()}};
  auto archived = record;
  archived["kind"] = "raw";
  if (!Write(archived)) {
    Finish(write_error_, true, wall);
    return;
  }
  ++frames_;
  raw_bytes_ += frame->wire.size();
  if ((frame->flags & 1) == 0) ++bad_checksum_;
  if (frames_batch_.empty()) batch_started_ = now;
  frames_batch_.push_back(record);
  if (Batch(wall).dump().size() > kBatchBytes) {
    frames_batch_.erase(frames_batch_.end() - 1);
    if (!Flush(wall)) {
      Finish("batch_queue_full", true, wall);
      return;
    }
    batch_started_ = now;
    frames_batch_.push_back(std::move(record));
  }
  if (frames_batch_.size() == kBatchFrames && !Flush(wall)) {
    Finish("batch_queue_full", true, wall);
  }
}

void GnssExperiment::Tick(Wall::time_point wall, Steady::time_point now, bool live) {
  if (!active_) return;
  if (!live) {
    Finish("serial_disconnected", true, wall);
    return;
  }
  if (now >= deadline_) {
    Finish("duration_elapsed", false, wall);
    return;
  }
  assembler_.Tick(now);
  if (!frames_batch_.empty() && now - batch_started_ >= kBatchWait && !Flush(wall)) {
    Finish("batch_queue_full", true, wall);
  }
}

void GnssExperiment::TransportLost(std::string_view reason, Wall::time_point wall) {
  if (active_) Finish(reason, true, wall);
}

void GnssExperiment::Finish(std::string_view reason, bool failed, Wall::time_point wall) {
  active_ = false;
  state_ = failed ? "failed" : "completed";
  end_reason_ = reason;
  if (!Flush(wall)) {
    state_ = "failed";
    end_reason_ = "batch_queue_full";
    frames_batch_ = Json::array();
  }
  assembler_.Reset();
  if (!Write(Json{{"kind", "summary"}, {"observed_at", Utc(wall)}, {"gnss", Status()}})) {
    state_ = "failed";
    end_reason_ = write_error_;
  }
  archive_.close();
  if (archive_.fail()) {
    state_ = "failed";
    end_reason_ = "archive_write_failed";
  }
}

bool GnssExperiment::Write(const Json& record) {
  const auto line = record.dump() + '\n';
  // 普通记录提前留出终态空间；容量耗尽时仍能写下真实结束原因。
  const auto limit = record.at("kind") == "summary" ? kArchiveLimit : kArchiveLimit - kSummaryReserve;
  if (archive_bytes_ + line.size() > limit) {
    write_error_ = "archive_limit";
    return false;
  }
  archive_ << line;
  archive_.flush();
  if (!archive_) {
    write_error_ = "archive_write_failed";
    return false;
  }
  archive_bytes_ += line.size();
  return true;
}

Json GnssExperiment::Batch(Wall::time_point wall) const {
  return Json{{"schema_version", 1}, {"device_id", device_id_}, {"session_id", session_id_},
      {"capture_id", capture_id_}, {"batch_seq", batch_seq_ + 1},
      {"observed_at", Utc(wall)}, {"frames", frames_batch_}};
}

bool GnssExperiment::Flush(Wall::time_point wall) {
  if (frames_batch_.empty()) return true;
  if (batches_.size() == kQueueCapacity) return false;
  batches_.push_back(Publication{topic_namespace_ + "/" + device_id_ + "/gnss/raw/v1",
      Batch(wall).dump(), 1, false});
  ++batch_seq_;
  frames_batch_ = Json::array();
  return true;
}

const Publication* GnssExperiment::FrontBatch() const {
  return batches_.empty() ? nullptr : &batches_.front();
}

void GnssExperiment::ConfirmBatch() {
  if (!batches_.empty()) {
    batches_.pop_front();
    ++confirmed_batches_;
  }
}

Json GnssExperiment::Status() const {
  const auto& counters = assembler_.Stats();
  return Json{{"enabled", config_.enabled}, {"state", state_},
      {"capture_id", capture_id_.empty() ? Json(nullptr) : Json(capture_id_)},
      {"session_id", session_id_.empty() ? Json(nullptr) : Json(session_id_)},
      {"frames", frames_}, {"raw_bytes", raw_bytes_},
      {"source_checksum_bad_frames", bad_checksum_}, {"queued_batches", batches_.size()},
      {"confirmed_batches", confirmed_batches_}, {"end_reason", end_reason_},
      {"transport_counters", {{"scope", "process_lifetime"},
          {"fragments_received", counters.fragments_received}, {"frames_completed", counters.frames_completed},
          {"invalid_fragments", counters.invalid_fragments}, {"timed_out_frames", counters.timed_out_frames},
          {"conflicting_frames", counters.conflicting_frames}, {"wire_crc_errors", counters.wire_crc_errors},
          {"duplicate_fragments", counters.duplicate_fragments}, {"duplicate_frames", counters.duplicate_frames},
          {"evicted_frames", counters.evicted_frames}}}};
}
}  // namespace experiment
