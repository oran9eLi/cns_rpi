/**
 * @file m1_transaction.cpp
 * @brief M1 改参与探测单动作事务，先持久化身份后允许物理副作用。
 */

#include "experiment/m1_transaction.hpp"

#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <utility>

#include "experiment/m1_protocol.hpp"
#include "experiment/uart_echo_protocol.hpp"
#include "mqtt/topic.hpp"
#include "protocol/identity.hpp"

namespace experiment {
namespace {
using Json = nlohmann::json;
constexpr auto kResponseTimeout = std::chrono::seconds(5);
constexpr auto kProbeGap = std::chrono::milliseconds(300);
constexpr std::size_t kProbeCount = 3;
constexpr std::size_t kMaxRecords = 128;
constexpr std::size_t kMaxAckBytes = 4096;
constexpr std::size_t kMaxFrameBytes = 280;
constexpr std::string_view kProbeText = "CNS_TEST";

std::expected<std::uint64_t, std::string> RandomNonce() {
  std::uint64_t value{};
  if (::getrandom(&value, sizeof(value), 0) != sizeof(value) || value == 0) {
    return std::unexpected("无法取得M1动作随机数");
  }
  return value;
}

std::string OperationName(M1Operation operation) {
  switch (operation) {
    case M1Operation::kSetF407Baud: return "set_f407_baud";
    case M1Operation::kSetPiBaud: return "set_pi_baud";
    case M1Operation::kUartProbe: return "uart_probe";
  }
  return {};
}

std::string FormatTime(std::chrono::system_clock::time_point now) {
  const auto rounded = std::chrono::floor<std::chrono::milliseconds>(now);
  const auto day = std::chrono::floor<std::chrono::days>(rounded);
  const std::chrono::year_month_day date{day};
  const std::chrono::hh_mm_ss time{rounded - day};
  char output[32]{};
  std::snprintf(output, sizeof(output), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
                static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                static_cast<unsigned>(date.day()),
                static_cast<long long>(time.hours().count()),
                static_cast<long long>(time.minutes().count()),
                static_cast<long long>(time.seconds().count()),
                static_cast<long long>(time.subseconds().count()));
  return output;
}

Json BaseAck(const M1Request& request, std::chrono::system_clock::time_point now) {
  return {{"schema_version", 1}, {"command_id", request.command_id},
          {"session_id", request.session_id}, {"action_id", request.action_id},
          {"operation", OperationName(request.operation)},
          {"observed_at", FormatTime(now)}};
}

Json FailedAck(const M1Request& request, std::chrono::system_clock::time_point now,
               std::string_view code) {
  auto ack = BaseAck(request, now);
  ack["status"] = "rejected";
  ack["error_code"] = code;
  return ack;
}

std::string Comparison(const M1Request& request) {
  return Json{{"device_id", request.device_id},
              {"session_id", request.session_id},
              {"request_id", request.request_id},
              {"lease_version", request.lease_version},
              {"expires_at", request.expires_at},
              {"operation", OperationName(request.operation)},
              {"baud_rate", request.baud_rate}}.dump();
}

std::string NonceHex(std::uint64_t nonce) {
  char buffer[17]{};
  std::snprintf(buffer, sizeof(buffer), "%016llx",
                static_cast<unsigned long long>(nonce));
  return buffer;
}

std::expected<void, std::string> WriteAll(int fd, std::string_view content) {
  while (!content.empty()) {
    const auto count = ::write(fd, content.data(), content.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return std::unexpected("写入M1动作去重记录失败");
    content.remove_prefix(static_cast<std::size_t>(count));
  }
  return {};
}
}  // namespace

M1Transaction::M1Transaction(std::filesystem::path journal_path,
                             std::string topic_namespace,
                             NonceGenerator nonce_generator,
                             DirectorySync directory_sync,
                             ForeignActionLookup foreign_action_lookup)
    : journal_path_(std::move(journal_path)),
      topic_namespace_(std::move(topic_namespace)),
      nonce_generator_(nonce_generator ? std::move(nonce_generator) : RandomNonce),
      directory_sync_(directory_sync ? std::move(directory_sync)
                                     : DirectorySync{[](int fd) { return ::fsync(fd); }}),
      foreign_action_lookup_(std::move(foreign_action_lookup)) {}

std::expected<void, std::string> M1Transaction::Load() {
  records_.clear();
  loaded_ = false;
  std::error_code error;
  if (!std::filesystem::exists(journal_path_, error)) {
    if (error) return std::unexpected("读取M1动作去重记录失败: " + error.message());
    loaded_ = true;
    storage_ambiguous_ = false;
    return {};
  }
  std::ifstream input(journal_path_, std::ios::binary);
  if (!input) return std::unexpected("打开M1动作去重记录失败");
  try {
    Json document = Json::parse(input);
    if (!document.is_object() || document.at("schema_version") != 1 ||
        !document.at("records").is_array() ||
        document.at("records").size() > kMaxRecords) {
      return std::unexpected("M1动作去重记录格式非法");
    }
    for (const auto& item : document.at("records")) {
      const auto expiry_ms = item.at("expiry_ms").get<std::int64_t>();
      const auto action_id = item.at("action_id").get<std::string>();
      const auto comparison = item.at("comparison").get<std::string>();
      const auto terminal = item.at("terminal_payload").get<std::string>();
      const Json identity = Json::parse(comparison);
      if (!protocol::IsValidUasId(identity.at("device_id").get<std::string>()) ||
          (!terminal.empty() && Json::parse(terminal).at("action_id") != action_id)) {
        return std::unexpected("M1动作去重记录身份不一致");
      }
      records_.push_back({action_id, comparison, terminal,
                          WallClock::time_point{std::chrono::milliseconds(expiry_ms)}});
    }
  } catch (const Json::exception&) {
    records_.clear();
    return std::unexpected("M1动作去重记录格式非法");
  }
  loaded_ = true;
  storage_ambiguous_ = false;
  return {};
}

bool M1Transaction::HasActionId(std::string_view action_id) const {
  return std::ranges::any_of(records_, [&](const Record& record) {
    return record.action_id == action_id;
  });
}

std::vector<Publication> M1Transaction::RecoveredPublications() const {
  std::vector<Publication> recovered;
  if (!loaded_) return recovered;
  for (const auto& record : records_) {
    if (record.terminal_payload.empty()) continue;
    const auto comparison = Json::parse(record.comparison);
    recovered.push_back(Publish(comparison.at("device_id").get<std::string>(),
                                record.terminal_payload));
  }
  return recovered;
}

std::string M1Transaction::TakeDiagnostic() {
  return std::exchange(storage_diagnostic_, {});
}

std::expected<void, std::string> M1Transaction::SaveRecords() {
  last_save_renamed_ = false;
  Json items = Json::array();
  for (const auto& record : records_) {
    items.push_back({{"action_id", record.action_id},
                     {"comparison", record.comparison},
                     {"terminal_payload", record.terminal_payload},
                     {"expiry_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                           record.expiry.time_since_epoch()).count()}});
  }
  const auto content = Json{{"schema_version", 1}, {"records", items}}.dump();
  const auto temporary = journal_path_.string() + ".tmp";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return std::unexpected("创建M1动作去重记录失败");
  const auto written = WriteAll(fd, content);
  const bool synced = written && ::fsync(fd) == 0;
  const bool closed = ::close(fd) == 0;
  if (!synced || !closed || ::rename(temporary.c_str(), journal_path_.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return std::unexpected("持久化M1动作去重记录失败");
  }
  last_save_renamed_ = true;
  const auto parent = journal_path_.parent_path().empty()
                          ? std::filesystem::path{"."} : journal_path_.parent_path();
  const int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd < 0) return std::unexpected("同步M1动作记录目录失败");
  const bool dir_synced = directory_sync_(dir_fd) == 0;
  ::close(dir_fd);
  if (!dir_synced) return std::unexpected("同步M1动作记录目录失败");
  return {};
}

Publication M1Transaction::Publish(const std::string& device_id,
                                   const std::string& payload) const {
  return {.topic = mqtt::BuildExperimentAckTopic(topic_namespace_, device_id),
          .payload = payload, .qos = 2, .retain = false};
}

M1Start M1Transaction::Start(const M1Request& request, M1Gate gate,
                             WallClock::time_point wall_now,
                             SteadyClock::time_point steady_now) {
  auto rejected = [&](std::string_view code, std::string diagnostic) {
    M1Start outcome;
    outcome.publication = Publish(request.device_id,
                                  FailedAck(request, wall_now, code).dump());
    outcome.diagnostic = std::move(diagnostic);
    return outcome;
  };
  if (!loaded_) {
    if (storage_ambiguous_) {
      M1Start outcome;
      outcome.diagnostic = "M1动作记录同步结果不明，需重新装载后再应答";
      return outcome;
    }
    return rejected("result_uncertain", "M1动作去重记录未成功加载");
  }
  const auto comparison = Comparison(request);
  for (const auto& record : records_) {
    if (record.action_id != request.action_id) continue;
    if (record.comparison != comparison) {
      return rejected("duplicate_conflict", "M1动作ID重复且内容冲突");
    }
    if (record.terminal_payload.empty()) {
      if (pending_ && pending_->request.action_id == request.action_id) {
        M1Start outcome;
        outcome.diagnostic = "M1动作仍在执行，未再次执行";
        return outcome;
      }
      return rejected("result_uncertain", "M1动作曾准备执行，重启后结果待核验");
    }
    auto ack = Json::parse(record.terminal_payload);
    ack["command_id"] = request.command_id;
    M1Start outcome;
    outcome.publication = Publish(request.device_id, ack.dump());
    return outcome;
  }
  if (foreign_action_lookup_ && foreign_action_lookup_(request.action_id)) {
    return rejected("duplicate_conflict", "动作ID已由其他实验操作占用");
  }
  auto durable_rejected = [&](std::string_view code, std::string diagnostic) {
    if (records_.size() >= kMaxRecords) {
      return rejected("result_uncertain", "M1动作去重记录已达上限");
    }
    auto outcome = rejected(code, std::move(diagnostic));
    records_.push_back({request.action_id, comparison,
                        outcome.publication->payload, request.expiry});
    if (const auto saved = SaveRecords(); !saved) {
      loaded_ = false;
      if (last_save_renamed_) {
        storage_ambiguous_ = true;
        storage_diagnostic_ = "M1拒绝终态目录同步失败，结果暂不发布: " + saved.error();
        outcome.publication.reset();
        outcome.diagnostic = storage_diagnostic_;
        return outcome;
      }
      records_.pop_back();
      return rejected("result_uncertain", saved.error());
    }
    return outcome;
  };
  if (wall_now >= request.expiry) {
    return durable_rejected("action_expired", "M1动作已过期");
  }
  if (!gate.mqtt_connected || !gate.cns_box || !gate.persisted_binding) {
    return durable_rejected("identity_unavailable", "主控箱固定绑定或MQTT控制不可用");
  }
  if ((request.operation == M1Operation::kSetF407Baud &&
       request.baud_rate != 57600) ||
      (request.operation == M1Operation::kSetPiBaud &&
       request.baud_rate != 57600 && request.baud_rate != 115200) ||
      (request.operation == M1Operation::kUartProbe && request.baud_rate)) {
    return durable_rejected("invalid_request", "M1动作目标波特率不符合约定");
  }
  if (pending_ || gate.serial_busy) {
    return durable_rejected("serial_unavailable", "串口下行动作仍在进行");
  }
  if (request.operation == M1Operation::kSetF407Baud) {
    if (!gate.identity_verified || !gate.trusted_endpoint ||
        !gate.serial_open || gate.pi_baud != 115200 ||
        gate.source_system == 0 || gate.source_component == 0 ||
        gate.target_system == 0 || gate.target_component == 0) {
      return durable_rejected("identity_unavailable", "F407当前身份或基准串口未核验");
    }
  } else if (request.operation == M1Operation::kUartProbe) {
    if (!gate.trusted_endpoint || !gate.serial_open || gate.pi_baud != 115200 ||
        gate.source_system == 0 || gate.source_component == 0 ||
        gate.target_system == 0 || gate.target_component == 0) {
      return durable_rejected("serial_unavailable", "失配探测缺少可信串口端点");
    }
  } else if (!gate.trusted_endpoint) {
    return durable_rejected("serial_unavailable", "Pi在线改参缺少可信串口路径");
  }
  if (records_.size() >= kMaxRecords) {
    return rejected("result_uncertain", "M1动作去重记录已达上限");
  }
  std::optional<std::uint64_t> nonce;
  std::optional<mavlink_message_t> outbound;
  if (request.operation == M1Operation::kSetF407Baud) {
    auto generated = nonce_generator_();
    if (!generated || *generated == 0) {
      return durable_rejected("result_uncertain", "无法取得F407改参随机数");
    }
    nonce = *generated;
    auto encoded = EncodeBaudChangeRequest(
        *nonce, static_cast<std::uint32_t>(*request.baud_rate),
        gate.source_system, gate.source_component,
        gate.target_system, gate.target_component);
    if (!encoded) return durable_rejected("result_uncertain", encoded.error());
    outbound = *encoded;
  } else if (request.operation == M1Operation::kUartProbe) {
    auto generated = nonce_generator_();
    if (!generated || *generated == 0) {
      return durable_rejected("result_uncertain", "无法取得失配探测随机数");
    }
    nonce = *generated;
    const auto text = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(kProbeText.data()), kProbeText.size());
    auto encoded = EncodeEchoRequest(text, *nonce, gate.source_system,
                                     gate.source_component, gate.target_system,
                                     gate.target_component);
    if (!encoded) return durable_rejected("result_uncertain", encoded.error());
    outbound = *encoded;
  }
  records_.push_back({request.action_id, comparison, "", request.expiry});
  if (const auto saved = SaveRecords(); !saved) {
    loaded_ = false;
    if (last_save_renamed_) {
      storage_ambiguous_ = true;
      storage_diagnostic_ = "M1动作准备记录同步失败，已禁止物理动作: " + saved.error();
      M1Start outcome;
      outcome.diagnostic = storage_diagnostic_;
      return outcome;
    }
    records_.pop_back();
    return rejected("result_uncertain", saved.error());
  }
  pending_ = Pending{.request = request, .gate = gate,
                     .nonce = nonce.value_or(0), .sent_at = {},
                     .next_send_at = steady_now, .sent = false,
                     .tx_bytes = {}, .acceptance_frame = std::nullopt,
                     .applied_pi_baud = std::nullopt, .samples = {}};
  M1Start outcome;
  outcome.outbound = outbound;
  if (request.operation == M1Operation::kUartProbe) {
    pending_->samples.push_back(ProbeSample{
        .nonce = *nonce, .tx_bytes = {}, .response = std::nullopt,
        .round_trip_us = std::nullopt});
  } else if (request.operation == M1Operation::kSetPiBaud) {
    outcome.apply_pi_baud = request.baud_rate;
  }
  return outcome;
}

M1Step M1Transaction::OnSent(const uart::SentFrame& sent,
                             WallClock::time_point wall_now) {
  if (!pending_ || pending_->sent ||
      pending_->request.operation == M1Operation::kSetPiBaud) return {};
  if (sent.bytes.empty() || sent.bytes.size() > kMaxFrameBytes) {
    return Finish("raw_frame_missing", wall_now);
  }
  pending_->sent_at = sent.started_at;
  pending_->sent = true;
  if (pending_->request.operation == M1Operation::kUartProbe) {
    pending_->samples.back().tx_bytes = sent.bytes;
  } else {
    pending_->tx_bytes = sent.bytes;
  }
  return {};
}

M1Step M1Transaction::OnFrame(const uart::WireFrame& frame,
                              WallClock::time_point wall_now) {
  if (!pending_ || !pending_->sent || frame.received_at < pending_->sent_at ||
      frame.message.sysid != pending_->gate.target_system ||
      frame.message.compid != pending_->gate.target_component ||
      frame.bytes.empty() || frame.bytes.size() > kMaxFrameBytes) return {};
  if (pending_->request.operation == M1Operation::kSetF407Baud) {
    const auto accepted = DecodeBaudChangeAccepted(
        frame.message, pending_->gate.target_system,
        pending_->gate.target_component, pending_->gate.source_system,
        pending_->gate.source_component, pending_->nonce,
        static_cast<std::uint32_t>(*pending_->request.baud_rate));
    if (!accepted) return {};
    pending_->acceptance_frame = frame;
    return Finish("", wall_now);
  }
  if (pending_->request.operation != M1Operation::kUartProbe) return {};
  const auto response = DecodeEchoResponse(
      frame.message, pending_->gate.source_system,
      pending_->gate.source_component);
  const auto expected_text = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(kProbeText.data()), kProbeText.size());
  if (!response || response->nonce != pending_->nonce ||
      response->text_bytes != std::vector<std::uint8_t>(expected_text.begin(),
                                                        expected_text.end()) ||
      response->f407_baud_rate != static_cast<std::uint32_t>(pending_->gate.pi_baud)) {
    return {};
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      frame.received_at - pending_->sent_at).count();
  if (elapsed < 0) return {};
  auto& sample = pending_->samples.back();
  sample.response = frame;
  sample.round_trip_us = elapsed;
  pending_->sent = false;
  pending_->next_send_at = pending_->sent_at + kProbeGap;
  return AdvanceProbe(frame.received_at, wall_now);
}

M1Step M1Transaction::AdvanceProbe(SteadyClock::time_point now,
                                    WallClock::time_point wall_now) {
  if (!pending_ || pending_->request.operation != M1Operation::kUartProbe ||
      pending_->sent || now < pending_->next_send_at) return {};
  if (pending_->samples.size() >= kProbeCount) return Finish("", wall_now);
  auto nonce = nonce_generator_();
  if (!nonce || *nonce == 0) return Finish("result_uncertain", wall_now);
  const auto text = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(kProbeText.data()), kProbeText.size());
  auto outbound = EncodeEchoRequest(text, *nonce, pending_->gate.source_system,
                                    pending_->gate.source_component,
                                    pending_->gate.target_system,
                                    pending_->gate.target_component);
  if (!outbound) return Finish("result_uncertain", wall_now);
  pending_->nonce = *nonce;
  pending_->samples.push_back(ProbeSample{
      .nonce = *nonce, .tx_bytes = {}, .response = std::nullopt,
      .round_trip_us = std::nullopt});
  return {.outbound = *outbound, .publication = std::nullopt};
}

M1Step M1Transaction::Tick(SteadyClock::time_point now,
                            WallClock::time_point wall_now) {
  if (!pending_) return {};
  if (pending_->request.operation == M1Operation::kSetF407Baud) {
    if (pending_->sent && now - pending_->sent_at >= kResponseTimeout) {
      return Finish("result_uncertain", wall_now);
    }
    return {};
  }
  if (pending_->request.operation == M1Operation::kUartProbe) {
    if (pending_->sent && now - pending_->sent_at >= kResponseTimeout) {
      pending_->sent = false;
      pending_->next_send_at = pending_->sent_at + kProbeGap;
    }
    return AdvanceProbe(now, wall_now);
  }
  return {};
}

M1Step M1Transaction::Abort(std::string_view error_code,
                             WallClock::time_point wall_now) {
  if (!pending_) return {};
  if (pending_->request.operation == M1Operation::kSetF407Baud && pending_->sent) {
    return Finish("result_uncertain", wall_now);
  }
  return Finish(error_code, wall_now);
}

std::optional<Publication> M1Transaction::OnPiBaudApplied(
    int applied_baud, bool serial_port_open, WallClock::time_point wall_now) {
  if (!pending_ || pending_->request.operation != M1Operation::kSetPiBaud) {
    return std::nullopt;
  }
  if (!serial_port_open || applied_baud != pending_->request.baud_rate) {
    return Finish("serial_reconfigure_failed", wall_now).publication;
  }
  pending_->applied_pi_baud = applied_baud;
  return Finish("", wall_now).publication;
}

std::optional<Publication> M1Transaction::OnPiBaudFailed(
    std::string_view error_code, WallClock::time_point wall_now) {
  if (!pending_ || pending_->request.operation != M1Operation::kSetPiBaud) {
    return std::nullopt;
  }
  if (error_code != "config_write_failed" &&
      error_code != "serial_reconfigure_failed") {
    error_code = "result_uncertain";
  }
  return Finish(error_code, wall_now).publication;
}

M1Step M1Transaction::Finish(std::string_view error_code,
                              WallClock::time_point wall_now) {
  if (!pending_) return {};
  Json ack;
  if (!error_code.empty()) {
    ack = FailedAck(pending_->request, wall_now, error_code);
  } else {
    ack = BaseAck(pending_->request, wall_now);
    ack["status"] = "completed";
    if (pending_->request.operation == M1Operation::kSetF407Baud) {
      if (!pending_->acceptance_frame || pending_->tx_bytes.empty()) {
        ack = FailedAck(pending_->request, wall_now, "result_uncertain");
      } else {
        const auto& rx = *pending_->acceptance_frame;
        ack["fact"] = {{"nonce", NonceHex(pending_->nonce)},
                       {"accepted_baud_rate", *pending_->request.baud_rate},
                       {"pi_baud_rate", pending_->gate.pi_baud},
                       {"tx_frame_hex", FrameHex(pending_->tx_bytes)},
                       {"tx_frame_bytes", pending_->tx_bytes.size()},
                       {"rx_frame_hex", FrameHex(rx.bytes)},
                       {"rx_frame_bytes", rx.bytes.size()}};
      }
    } else if (pending_->request.operation == M1Operation::kSetPiBaud) {
      if (!pending_->applied_pi_baud) {
        ack = FailedAck(pending_->request, wall_now, "result_uncertain");
      } else {
        ack["fact"] = {{"pi_baud_rate", *pending_->applied_pi_baud},
                       {"serial_port_open", true}};
      }
    } else if (pending_->samples.size() != kProbeCount) {
      ack = FailedAck(pending_->request, wall_now, "result_uncertain");
    } else {
      Json samples = Json::array();
      int valid_count = 0;
      for (const auto& sample : pending_->samples) {
        Json row{{"nonce", NonceHex(sample.nonce)},
                 {"tx_frame_hex", FrameHex(sample.tx_bytes)},
                 {"tx_frame_bytes", sample.tx_bytes.size()},
                 {"valid_rx_frame_hex", nullptr},
                 {"rx_frame_bytes", nullptr},
                 {"round_trip_us", nullptr}};
        if (sample.tx_bytes.empty()) {
          ack = FailedAck(pending_->request, wall_now, "serial_unavailable");
          break;
        }
        if (sample.response && sample.round_trip_us) {
          row["valid_rx_frame_hex"] = FrameHex(sample.response->bytes);
          row["rx_frame_bytes"] = sample.response->bytes.size();
          row["round_trip_us"] = *sample.round_trip_us;
          ++valid_count;
        }
        samples.push_back(std::move(row));
      }
      if (ack.value("status", std::string{}) == "completed") {
        ack["fact"] = {{"pi_baud_rate", pending_->gate.pi_baud},
                       {"attempted_count", 3},
                       {"valid_response_count", valid_count},
                       {"samples", std::move(samples)}};
      }
    }
  }
  if (ack.dump().size() > kMaxAckBytes) {
    ack = FailedAck(pending_->request, wall_now, "result_uncertain");
  }
  const auto publication = Publish(pending_->request.device_id, ack.dump());
  const auto existing = std::find_if(records_.begin(), records_.end(),
                                     [&](const Record& record) {
    return record.action_id == pending_->request.action_id;
  });
  if (existing != records_.end()) {
    existing->terminal_payload = publication.payload;
    if (const auto saved = SaveRecords(); !saved) {
      loaded_ = false;
      storage_diagnostic_ = "M1终态持久化失败，已禁止后续动作: " + saved.error();
      if (last_save_renamed_) {
        storage_ambiguous_ = true;
        pending_.reset();
        return {};
      }
      existing->terminal_payload.clear();
      const auto uncertain = Publish(
          pending_->request.device_id,
          FailedAck(pending_->request, wall_now, "result_uncertain").dump());
      pending_.reset();
      return {.outbound = std::nullopt, .publication = uncertain};
    }
  }
  pending_.reset();
  return {.outbound = std::nullopt, .publication = publication};
}

bool M1Transaction::HasPending() const { return pending_.has_value(); }

}  // namespace experiment
