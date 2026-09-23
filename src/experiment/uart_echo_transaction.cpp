/** @file uart_echo_transaction.cpp
 * @brief H1 单动作、先落盘后发帧及真实回显事实核验。
 */

#include "experiment/uart_echo_transaction.hpp"

#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string_view>

#include "experiment/uart_echo_protocol.hpp"
#include "mqtt/topic.hpp"

namespace experiment {
namespace {
using Json = nlohmann::json;
constexpr auto kRetention = std::chrono::seconds(300);
constexpr auto kResponseTimeout = std::chrono::seconds(5);
constexpr auto kActionTimeout = std::chrono::seconds(10);
constexpr std::size_t kMaxRecords = 128;
constexpr std::size_t kMaxAckBytes = 4096;

std::expected<std::uint64_t, std::string> RandomNonce() {
  std::uint64_t value{};
  if (::getrandom(&value, sizeof(value), 0) != sizeof(value) || value == 0) {
    return std::unexpected("无法取得回显动作随机数");
  }
  return value;
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

Json BaseAck(const UartEchoRequest& request,
             std::chrono::system_clock::time_point now) {
  return {{"schema_version", 1}, {"command_id", request.command_id},
          {"session_id", request.session_id}, {"action_id", request.action_id},
          {"operation", "uart_echo"}, {"observed_at", FormatTime(now)}};
}

Json FailedAck(const UartEchoRequest& request,
               std::chrono::system_clock::time_point now,
               std::string_view code, std::string_view status = "rejected") {
  auto ack = BaseAck(request, now);
  ack["status"] = status;
  ack["error_code"] = code;
  return ack;
}

std::string Comparison(const UartEchoRequest& request) {
  return Json{{"device_id", request.device_id},
              {"session_id", request.session_id},
              {"request_id", request.request_id},
              {"lease_version", request.lease_version},
              {"expires_at", request.expires_at},
              {"text", request.text_bytes}}.dump();
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
    if (count <= 0) return std::unexpected("写入动作去重记录失败");
    content.remove_prefix(static_cast<std::size_t>(count));
  }
  return {};
}
}  // namespace

UartEchoTransaction::UartEchoTransaction(
    std::filesystem::path journal_path, std::string topic_namespace,
    NonceGenerator nonce_generator)
    : journal_path_(std::move(journal_path)),
      topic_namespace_(std::move(topic_namespace)),
      nonce_generator_(nonce_generator ? std::move(nonce_generator) : RandomNonce) {}

std::expected<void, std::string> UartEchoTransaction::Load() {
  records_.clear();
  loaded_ = false;
  std::error_code error;
  if (!std::filesystem::exists(journal_path_, error)) {
    if (error) return std::unexpected("读取动作去重记录失败: " + error.message());
    loaded_ = true;
    return {};
  }
  std::ifstream input(journal_path_, std::ios::binary);
  if (!input) return std::unexpected("打开动作去重记录失败");
  try {
    Json document = Json::parse(input);
    if (!document.is_object() || document.at("schema_version") != 1 ||
        !document.at("records").is_array() ||
        document.at("records").size() > kMaxRecords) {
      return std::unexpected("动作去重记录格式非法");
    }
    for (const auto& record : document.at("records")) {
      const auto expiry_ms = record.at("expiry_ms").get<std::int64_t>();
      records_.push_back({record.at("action_id").get<std::string>(),
                          record.at("comparison").get<std::string>(),
                          record.at("terminal_payload").get<std::string>(),
                          WallClock::time_point{std::chrono::milliseconds(expiry_ms)}});
    }
  } catch (const Json::exception&) {
    records_.clear();
    return std::unexpected("动作去重记录格式非法");
  }
  loaded_ = true;
  return {};
}

std::expected<void, std::string> UartEchoTransaction::SaveRecords() const {
  Json items = Json::array();
  for (const auto& record : records_) {
    items.push_back({{"action_id", record.action_id},
                     {"comparison", record.comparison},
                     {"terminal_payload", record.terminal_payload},
                     {"expiry_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                           record.expiry.time_since_epoch()).count()}});
  }
  const std::string content = Json{{"schema_version", 1}, {"records", items}}.dump();
  const auto temporary = journal_path_.string() + ".tmp";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return std::unexpected("创建动作去重记录失败");
  const auto written = WriteAll(fd, content);
  const bool synced = written && ::fsync(fd) == 0;
  const bool closed = ::close(fd) == 0;
  if (!synced || !closed || ::rename(temporary.c_str(), journal_path_.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return std::unexpected("持久化动作去重记录失败");
  }
  const auto parent = journal_path_.parent_path().empty()
                          ? std::filesystem::path{"."}
                          : journal_path_.parent_path();
  const int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd < 0) return std::unexpected("同步动作记录目录失败");
  const bool dir_synced = ::fsync(dir_fd) == 0;
  ::close(dir_fd);
  if (!dir_synced) return std::unexpected("同步动作记录目录失败");
  return {};
}

Publication UartEchoTransaction::Publish(const std::string& device_id,
                                          const std::string& payload) const {
  return {.topic = mqtt::BuildExperimentAckTopic(topic_namespace_, device_id),
          .payload = payload, .qos = 2, .retain = false};
}

EchoStart UartEchoTransaction::Start(const UartEchoRequest& request, EchoGate gate,
                                      WallClock::time_point wall_now,
                                      SteadyClock::time_point steady_now) {
  auto rejected = [&](std::string_view code, std::string message) -> EchoStart {
    return {.outbound = std::nullopt,
            .publication = Publish(request.device_id,
                                   FailedAck(request, wall_now, code).dump()),
            .diagnostic = std::move(message)};
  };
  if (!loaded_) return rejected("result_uncertain", "动作去重记录未成功加载");
  const auto comparison = Comparison(request);
  for (const auto& record : records_) {
    if (record.action_id != request.action_id) continue;
    if (record.comparison != comparison) {
      return rejected("duplicate_conflict", "回显动作ID重复且内容冲突");
    }
    if (record.terminal_payload.empty()) {
      if (pending_ && pending_->request.action_id == request.action_id) {
        return {.outbound = std::nullopt, .publication = std::nullopt,
                .diagnostic = "回显动作仍在执行，未再次发送"};
      }
      return rejected("result_uncertain", "动作曾准备发送，重启后无法确认执行结果");
    }
    auto ack = Json::parse(record.terminal_payload);
    ack["command_id"] = request.command_id;
    return {.outbound = std::nullopt,
            .publication = Publish(request.device_id, ack.dump()), .diagnostic = {}};
  }
  if (wall_now >= request.expiry) return rejected("action_expired", "回显动作已过期");
  if (pending_ || gate.serial_busy) {
    return rejected("serial_unavailable", "串口下行动作仍在进行");
  }
  if (!gate.cns_box || !gate.identity_verified ||
      gate.source_system == 0 || gate.target_system == 0 ||
      gate.target_component == 0) {
    return rejected("identity_unavailable", "F407当前身份尚未核验");
  }
  if (!gate.serial_open || gate.pi_baud <= 0) {
    return rejected("serial_unavailable", "串口端点或实际波特率不可用");
  }
  auto nonce = nonce_generator_();
  if (!nonce || *nonce == 0) {
    return rejected("result_uncertain", "无法取得不可预测随机数");
  }
  auto outbound = EncodeEchoRequest(request.text_bytes, *nonce,
                                    gate.source_system, gate.source_component,
                                    gate.target_system, gate.target_component);
  if (!outbound) return rejected("payload_mismatch", outbound.error());
  records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const Record& record) {
    return wall_now > record.expiry + kRetention;
  }), records_.end());
  if (records_.size() >= kMaxRecords) {
    return rejected("result_uncertain", "动作去重记录已达上限");
  }
  records_.push_back({request.action_id, comparison, "", request.expiry});
  if (const auto saved = SaveRecords(); !saved) {
    records_.pop_back();
    loaded_ = false;
    return rejected("result_uncertain", saved.error());
  }
  pending_ = Pending{.request = request, .comparison = comparison,
                     .nonce = *nonce, .gate = gate, .started_at = steady_now,
                     .sent_at = {}, .sent = false, .fresh_business_frame = false,
                     .tx_bytes = {}, .response_frame = std::nullopt,
                     .response_text_bytes = {},
                     .f407_baud = std::nullopt};
  return {.outbound = *outbound, .publication = std::nullopt, .diagnostic = {}};
}

std::optional<Publication> UartEchoTransaction::OnSent(
    const uart::SentFrame& sent, WallClock::time_point wall_now) {
  if (!pending_) return std::nullopt;
  if (sent.bytes.empty() || sent.bytes.size() > MAVLINK_MAX_PACKET_LEN) {
    return Finish("raw_frame_missing", wall_now);
  }
  pending_->tx_bytes = sent.bytes;
  pending_->sent_at = sent.started_at;
  pending_->sent = true;
  return std::nullopt;
}

std::optional<Publication> UartEchoTransaction::OnFrame(
    const uart::WireFrame& frame, WallClock::time_point wall_now) {
  if (!pending_ || !pending_->sent ||
      frame.received_at < pending_->sent_at ||
      frame.message.msgid != MAVLINK_MSG_ID_TUNNEL ||
      frame.message.sysid != pending_->gate.target_system ||
      frame.message.compid != pending_->gate.target_component) return std::nullopt;
  mavlink_tunnel_t tunnel{};
  mavlink_msg_tunnel_decode(&frame.message, &tunnel);
  if (tunnel.payload_type != 0x8003) return std::nullopt;
  auto response = DecodeEchoResponse(frame.message, pending_->gate.source_system,
                                     pending_->gate.source_component);
  if (!response) return Finish("payload_mismatch", wall_now);
  if (response->nonce != pending_->nonce) return Finish("nonce_mismatch", wall_now);
  if (response->text_bytes != pending_->request.text_bytes) {
    return Finish("payload_mismatch", wall_now);
  }
  if (response->f407_baud_rate != static_cast<std::uint32_t>(pending_->gate.pi_baud)) {
    return Finish("baud_mismatch", wall_now);
  }
  if (frame.bytes.empty() || frame.bytes.size() > MAVLINK_MAX_PACKET_LEN) {
    return Finish("raw_frame_missing", wall_now);
  }
  pending_->response_frame = frame;
  pending_->response_text_bytes = response->text_bytes;
  pending_->f407_baud = response->f407_baud_rate;
  if (pending_->fresh_business_frame) return Finish("", wall_now);
  return std::nullopt;
}

std::optional<Publication> UartEchoTransaction::OnBusinessFrame(
    SteadyClock::time_point received_at, WallClock::time_point wall_now) {
  if (!pending_ || !pending_->sent || received_at < pending_->sent_at) {
    return std::nullopt;
  }
  pending_->fresh_business_frame = true;
  if (pending_->response_frame) return Finish("", wall_now);
  return std::nullopt;
}

std::optional<Publication> UartEchoTransaction::Tick(
    SteadyClock::time_point now, WallClock::time_point wall_now) {
  if (!pending_) return std::nullopt;
  if (!pending_->response_frame && now - pending_->sent_at >= kResponseTimeout) {
    return Finish("f407_timeout", wall_now);
  }
  if (now - pending_->sent_at >= kActionTimeout) {
    return Finish("fresh_business_frame_missing", wall_now);
  }
  return std::nullopt;
}

std::optional<Publication> UartEchoTransaction::Abort(
    std::string_view error_code, WallClock::time_point wall_now) {
  if (!pending_) return std::nullopt;
  return Finish(error_code, wall_now);
}

std::optional<Publication> UartEchoTransaction::Finish(
    std::string_view error_code, WallClock::time_point wall_now) {
  if (!pending_) return std::nullopt;
  Json ack;
  if (!error_code.empty()) {
    ack = FailedAck(pending_->request, wall_now, error_code);
  } else if (!pending_->response_frame || !pending_->f407_baud ||
             pending_->tx_bytes.empty() || !pending_->fresh_business_frame) {
    ack = FailedAck(pending_->request, wall_now, "result_uncertain");
  } else {
    const auto& rx = *pending_->response_frame;
    const auto rtt = std::chrono::duration_cast<std::chrono::microseconds>(
        rx.received_at - pending_->sent_at).count();
    if (rtt < 0) {
      ack = FailedAck(pending_->request, wall_now, "result_uncertain");
    } else {
      ack = BaseAck(pending_->request, wall_now);
      ack["status"] = "completed";
      const std::string text(pending_->request.text_bytes.begin(),
                             pending_->request.text_bytes.end());
      const std::string returned_text(pending_->response_text_bytes.begin(),
                                      pending_->response_text_bytes.end());
      ack["fact"] = {{"nonce", NonceHex(pending_->nonce)},
                     {"request_text", text}, {"response_text", returned_text},
                     {"pi_baud_rate", pending_->gate.pi_baud},
                     {"f407_baud_rate", *pending_->f407_baud},
                     {"fresh_business_frame", true},
                     {"tx_frame_hex", FrameHex(pending_->tx_bytes)},
                     {"tx_frame_bytes", pending_->tx_bytes.size()},
                     {"rx_frame_hex", FrameHex(rx.bytes)},
                     {"rx_frame_bytes", rx.bytes.size()},
                     {"round_trip_us", rtt}};
    }
  }
  if (ack.dump().size() > kMaxAckBytes) {
    ack = FailedAck(pending_->request, wall_now, "result_uncertain");
  }
  const auto publication = Publish(pending_->request.device_id, ack.dump());
  const auto existing = std::find_if(records_.begin(), records_.end(), [&](const Record& record) {
    return record.action_id == pending_->request.action_id;
  });
  if (existing != records_.end()) {
    existing->terminal_payload = publication.payload;
    (void)SaveRecords();
  }
  pending_.reset();
  return publication;
}

bool UartEchoTransaction::HasPending() const { return pending_.has_value(); }
}  // namespace experiment
