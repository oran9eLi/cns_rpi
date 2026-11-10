/**
 * @file base_station.cpp
 * @brief F9P USB 基站的混合字节流、只读轮询及异步 RAM 配置。
 * @details 协议依据：u-blox UBX-21023318 R02 (27.12)、UBX-22008968 R01
 * (27.31)、 UBXDOC-963802114-12815 R01 (27.50)。 不做 GNSS 解算、不写
 * Flash、不碰 UART1，也不把 RTCM 输出当成观测完成。
 */
#include "rtk/base_station.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "uart/serial_port.hpp"

namespace rtk {
namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using Bytes = std::vector<std::uint8_t>;
using Json = nlohmann::json;
using namespace std::chrono_literals;
constexpr std::size_t kMaxUbxPayload = 4096;
constexpr std::uint32_t kGpsWeekMs = 604800000;

std::uint64_t ReadLe(std::span<const std::uint8_t> bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i)
    value |= std::uint64_t{bytes[i]} << (8 * i);
  return value;
}

void AppendLe(Bytes &bytes, std::uint64_t value, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) bytes.push_back(value >> (8 * i));
}

Bytes Ubx(std::uint8_t cls, std::uint8_t id,
          std::span<const std::uint8_t> payload = {}) {
  Bytes bytes{0xb5, 0x62, cls, id};
  AppendLe(bytes, payload.size(), 2);
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  std::uint8_t a = 0, b = 0;
  for (std::size_t i = 2; i < bytes.size(); ++i) {
    a += bytes[i];
    b += a;
  }
  bytes.push_back(a);
  bytes.push_back(b);
  return bytes;
}

std::uint32_t Crc24q(std::span<const std::uint8_t> bytes) {
  std::uint32_t crc = 0;
  for (const auto byte : bytes) {
    crc ^= std::uint32_t{byte} << 16;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc << 1) ^ ((crc & 0x800000) ? 0x1864cfb : 0);
  }
  return crc & 0xffffff;
}

std::string Text(std::span<const std::uint8_t> bytes) {
  return {bytes.begin(), std::find(bytes.begin(), bytes.end(), 0)};
}

Json Age(const std::optional<Time> &timestamp, Time now) {
  if (!timestamp) return nullptr;
  return std::max<std::int64_t>(
      0, std::chrono::duration_cast<std::chrono::milliseconds>(now - *timestamp)
             .count());
}

bool Stale(const std::optional<Time> &timestamp, Time now,
           std::uint32_t limit_ms) {
  return !timestamp || now - *timestamp >= std::chrono::milliseconds(limit_ms);
}

int Hex(std::uint8_t ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  return -1;
}

std::size_t KeySize(std::uint32_t key) {
  switch (key >> 28) {
    case 1:
    case 2:
      return 1;
    case 3:
      return 2;
    case 4:
      return 4;
    case 5:
      return 8;
    default:
      return 0;
  }
}

using ConfigValues = std::map<std::uint32_t, std::uint64_t>;
constexpr std::uint32_t kTimeMode = 0x20030001;
constexpr std::uint32_t kMinDuration = 0x40030010;
constexpr std::uint32_t kAccuracy = 0x40030011;

ConfigValues SurveyConfig(std::uint32_t duration_s, double accuracy_m) {
  // 仅启用 USB 的 UBX/RTCM 及观测消息，保留 UART1 和 NMEA 现场设置。
  return {{0x10770001, 1},
          {0x10780001, 1},
          {0x10780004, 1},
          {kTimeMode, 1},
          {0x2091008b, 1},
          {0x209102c0, 1},
          {0x20910306, 5},
          {0x20910361, 1},
          {0x20910366, 1},
          {0x2091036b, 1},
          {0x20910370, 1},
          {kMinDuration, duration_s},
          {kAccuracy,
           static_cast<std::uint32_t>(std::llround(accuracy_m * 10000.0))}};
}

Bytes ConfigurationMessage(const ConfigValues &values, bool readback) {
  Bytes payload{0, static_cast<std::uint8_t>(readback ? 0 : 1), 0, 0};
  for (const auto &[key, value] : values) {
    AppendLe(payload, key, 4);
    if (!readback) AppendLe(payload, value, KeySize(key));
  }
  return Ubx(0x06, readback ? 0x8b : 0x8a, payload);
}

}  // namespace

struct BaseStation::Impl {
  explicit Impl(Settings value) : settings(std::move(value)) {}

  struct SurveyData {
    std::optional<Time> received_at, sampled_at;
    std::uint32_t tow_ms{}, duration_s{}, observations{};
    std::uint64_t sequence{};
    double x{}, y{}, z{}, accuracy_m{};
    bool active{}, valid{};
    std::string raw_hex;
  } survey;

  Settings settings;
  std::optional<uart::SerialPort> port;
  Time now{}, next_open{}, next_poll{};
  Bytes incoming;
  bool activation_barrier{false};
  std::size_t discard_prefix{};
  std::optional<Time> any_rx, version_rx, rtcm_rx, partial_since;
  std::string version, software, firmware, hardware, module, receiver_error,
      last_error;
  std::uint64_t rx_bytes{}, ubx_frames{}, ubx_checksum_errors{},
      framing_errors{};
  std::uint64_t rtcm_frames{}, rtcm_bytes{}, rtcm_crc_errors{}, nmea_frames{},
      nmea_errors{};
  std::map<std::string, std::uint64_t> rtcm_types;
  std::vector<RtcmFrame> pending_rtcm;
  std::uint64_t rtcm_sequence{}, rtcm_queue_dropped{};
  std::string configuration_state{"idle"};
  enum class Phase {
    kNone,
    kDisable,
    kStop,
    kSurvey,
    kReadback
  } phase{Phase::kNone};
  ConfigValues desired;
  Time deadline{};
  std::optional<Time> verified_at;
  std::optional<std::uint32_t> stop_tow;
  std::uint32_t requested_duration{};
  double requested_accuracy{};

  void Tick(Time tick);
  void Open();
  void Receive();
  void Parse();
  void HandleUbx(std::uint8_t cls, std::uint8_t id,
                 std::span<const std::uint8_t> payload,
                 std::span<const std::uint8_t> wire);
  void HandleVersion(std::span<const std::uint8_t> payload);
  void HandleSurvey(std::span<const std::uint8_t> payload,
                    std::span<const std::uint8_t> wire);
  void HandleAck(std::uint8_t id, std::span<const std::uint8_t> payload);
  void HandleReadback(std::span<const std::uint8_t> payload);
  void HandleNmea(std::span<const std::uint8_t> sentence);
  void AdvanceConfiguration();
  bool Send(const Bytes &bytes);
  void Disconnect(const std::string &error);
  void FailConfiguration(const std::string &error);
  bool ReceiverVerified(Time at) const;
  bool Busy() const;
  Json Status(Time at) const;
};

void BaseStation::Impl::Tick(Time tick) {
  now = tick;
  if (!port && now >= next_open) Open();
  if (!port) return;
  // 短状态/配置帧不应滞留一秒；丢失的半帧不能阻塞下一次响应。
  if (partial_since && now - *partial_since >= 1s) {
    incoming.clear();
    discard_prefix = 0;
    partial_since.reset();
    ++framing_errors;
  }
  Receive();
  if (!port) return;
  if (Busy() && !ReceiverVerified(now))
    FailConfiguration("receiver verification expired during configuration");
  if (Busy() && configuration_state != "pending" && now >= deadline)
    FailConfiguration(
        phase == Phase::kStop       ? "survey stop confirmation timed out"
        : phase == Phase::kReadback ? "configuration readback timed out"
                                    : "configuration ACK timed out");
  AdvanceConfiguration();
  if (port && now >= next_poll) {
    // 1秒新鲜度配置也需留出下一轮主循环和接收机应答的时间。
    next_poll =
        now + std::min(1000ms,
                       std::chrono::milliseconds(settings.stale_after_ms / 2));
    if (Send(Ubx(0x0a, 0x04))) Send(Ubx(0x01, 0x3b));
  }
}

void BaseStation::Impl::Open() {
  next_open = now + 1s;
  auto opened = uart::SerialPort::Open(settings.device, settings.baud);
  if (!opened) {
    last_error = "cannot open base serial endpoint (error " +
                 std::to_string(static_cast<int>(opened.error())) + ")";
    return;
  }
  port = std::move(*opened);
  next_poll = now;
  last_error.clear();
}

void BaseStation::Impl::Receive() {
  std::array<std::uint8_t, 4096> buffer{};
  // 每轮至多32 KiB；WaitReadable(0)避免 SerialPort 的100 ms空读等待。
  for (unsigned chunk = 0; chunk < 8; ++chunk) {
    const auto ready = port->WaitReadable(0ms);
    if (!ready) {
      Disconnect("base serial read failed");
      return;
    }
    if (!*ready) {
      if (activation_barrier) {
        activation_barrier = false;
        // 读空才开放新改正；仍缓存在解析器中的半帧属于授权前输入。
        discard_prefix = incoming.size();
      }
      return;
    }
    const auto count = port->Read(buffer);
    if (!count) {
      Disconnect("base serial read failed");
      return;
    }
    if (*count == 0) return;
    rx_bytes += *count;
    any_rx = now;
    incoming.insert(incoming.end(), buffer.begin(), buffer.begin() + *count);
    Parse();
  }
}

void BaseStation::Impl::Parse() {
  std::size_t used = 0;
  while (used < incoming.size()) {
    const auto data = std::span<const std::uint8_t>(incoming).subspan(used);
    if (data[0] == 0xb5) {
      if (data.size() < 2) break;
      if (data[1] != 0x62) {
        ++used;
        continue;
      }
      if (data.size() < 6) break;
      const auto length = ReadLe(data.subspan(4, 2));
      if (length > kMaxUbxPayload) {
        ++framing_errors;
        ++used;
        continue;
      }
      const auto total = 8 + length;
      if (data.size() < total) break;
      std::uint8_t a = 0, b = 0;
      for (std::size_t i = 2; i < total - 2; ++i) {
        a += data[i];
        b += a;
      }
      if (a != data[total - 2] || b != data[total - 1]) {
        ++ubx_checksum_errors;
        ++used;
        continue;
      }
      ++ubx_frames;
      HandleUbx(data[2], data[3], data.subspan(6, length), data.first(total));
      used += total;
    } else if (data[0] == 0xd3) {
      if (data.size() < 3) break;
      if ((data[1] & 0xfc) != 0) {
        ++used;
        continue;
      }
      const std::size_t length = ((data[1] & 3) << 8) | data[2];
      const auto total = length + 6;
      if (data.size() < total) break;
      const std::uint32_t crc = (std::uint32_t{data[total - 3]} << 16) |
                                (std::uint32_t{data[total - 2]} << 8) |
                                data[total - 1];
      if (Crc24q(data.first(total - 3)) != crc) {
        ++rtcm_crc_errors;
        ++used;
        continue;
      }
      ++rtcm_frames;
      rtcm_bytes += total;
      rtcm_rx = now;
      const auto type = static_cast<std::uint16_t>(
          length >= 2 ? (std::uint16_t{data[3]} << 4) | (data[4] >> 4) : 0);
      if (length >= 2) {
        ++rtcm_types[std::to_string(type)];
      }
      if (pending_rtcm.size() == 32) {
        pending_rtcm.erase(pending_rtcm.begin());
        ++rtcm_queue_dropped;
      }
      pending_rtcm.push_back({++rtcm_sequence, now, type,
                              Bytes(data.begin(), data.begin() + total),
                              !activation_barrier && used >= discard_prefix});
      used += total;
    } else if (data[0] == '$' || data[0] == '!') {
      const auto end = std::find(data.begin(), data.end(), '\n');
      const auto binary = std::find_if(data.begin() + 1, end, [](auto ch) {
        return (ch < 0x20 && ch != '\r') || ch > 0x7e;
      });
      if (binary != end) {
        ++used;
        continue;
      }
      if (end == data.end()) {
        if (data.size() < 1024) break;
        ++nmea_errors;
        ++used;
        continue;
      }
      const std::size_t size = end - data.begin() + 1;
      HandleNmea(data.first(size));
      used += size;
    } else
      ++used;
  }
  incoming.erase(incoming.begin(), incoming.begin() + used);
  discard_prefix = discard_prefix > used ? discard_prefix - used : 0;
  if (incoming.empty())
    partial_since.reset();
  else if (used || !partial_since)
    partial_since = now;
}

void BaseStation::Impl::HandleUbx(std::uint8_t cls, std::uint8_t id,
                                  std::span<const std::uint8_t> payload,
                                  std::span<const std::uint8_t> wire) {
  if (cls == 0x0a && id == 0x04)
    HandleVersion(payload);
  else if (cls == 0x01 && id == 0x3b)
    HandleSurvey(payload, wire);
  else if (cls == 0x06 && id == 0x8b)
    HandleReadback(payload);
  else if (cls == 0x05 && (id == 0 || id == 1))
    HandleAck(id, payload);
}

void BaseStation::Impl::HandleAck(std::uint8_t id,
                                  std::span<const std::uint8_t> payload) {
  if (payload.size() != 2 || payload[0] != 0x06) return;
  if (payload[1] == 0x8a && configuration_state == "awaiting_ack") {
    if (id == 0) {
      FailConfiguration("receiver rejected UBX-CFG-VALSET");
      return;
    }
    if (phase == Phase::kDisable) {
      // HPG 1.50 的停止动作在导航周期生效；ACK后立即启用会跳过该周期。
      if (survey.sampled_at) stop_tow = survey.tow_ms;
      phase = Phase::kStop;
      configuration_state = "awaiting_stop";
      deadline = now + 3s;
    } else {
      phase = Phase::kReadback;
      configuration_state = "pending";
    }
  } else if (payload[1] == 0x8b && id == 0 &&
             configuration_state == "awaiting_readback")
    FailConfiguration("receiver rejected UBX-CFG-VALGET");
}

void BaseStation::Impl::HandleVersion(std::span<const std::uint8_t> payload) {
  if (payload.size() < 40 || (payload.size() - 40) % 30 != 0) return;
  software = Text(payload.first(30));
  hardware = Text(payload.subspan(30, 10));
  version.clear();
  module.clear();
  firmware.clear();
  for (std::size_t i = 40; i < payload.size(); i += 30) {
    const auto extension = Text(payload.subspan(i, 30));
    if (extension.starts_with("PROTVER=")) version = extension.substr(8);
    if (extension.starts_with("MOD=")) module = extension.substr(4);
    if (extension.starts_with("FWVER=")) firmware = extension.substr(6);
  }
  version_rx = now;
  if (module != "ZED-F9P")
    receiver_error = "unsupported receiver module: " + module;
  else if (version != "27.12" && version != "27.31" && version != "27.50")
    receiver_error = "unsupported UBX protocol version: " + version;
  else
    receiver_error.clear();
}

void BaseStation::Impl::HandleSurvey(std::span<const std::uint8_t> payload,
                                     std::span<const std::uint8_t> wire) {
  if (payload.size() != 40 || payload[0] != 0 || payload[36] > 1 ||
      payload[37] > 1)
    return;
  const auto hp_x = std::bit_cast<std::int8_t>(payload[24]);
  const auto hp_y = std::bit_cast<std::int8_t>(payload[25]);
  const auto hp_z = std::bit_cast<std::int8_t>(payload[26]);
  if (std::abs(static_cast<int>(hp_x)) > 99 ||
      std::abs(static_cast<int>(hp_y)) > 99 ||
      std::abs(static_cast<int>(hp_z)) > 99)
    return;
  const auto tow = static_cast<std::uint32_t>(ReadLe(payload.subspan(4, 4)));
  if (tow >= kGpsWeekMs) return;
  if (!survey.sampled_at || tow != survey.tow_ms) {
    survey.sampled_at = now;
    ++survey.sequence;
  }
  constexpr char digits[] = "0123456789abcdef";
  survey.raw_hex.clear();
  for (const auto byte : wire) {
    survey.raw_hex += digits[byte >> 4];
    survey.raw_hex += digits[byte & 15];
  }
  survey.received_at = now;
  survey.tow_ms = tow;
  survey.duration_s = ReadLe(payload.subspan(8, 4));
  survey.x = std::bit_cast<std::int32_t>(
                 static_cast<std::uint32_t>(ReadLe(payload.subspan(12, 4)))) *
                 0.01 +
             hp_x * 0.0001;
  survey.y = std::bit_cast<std::int32_t>(
                 static_cast<std::uint32_t>(ReadLe(payload.subspan(16, 4)))) *
                 0.01 +
             hp_y * 0.0001;
  survey.z = std::bit_cast<std::int32_t>(
                 static_cast<std::uint32_t>(ReadLe(payload.subspan(20, 4)))) *
                 0.01 +
             hp_z * 0.0001;
  survey.accuracy_m = ReadLe(payload.subspan(28, 4)) * 0.0001;
  survey.observations = ReadLe(payload.subspan(32, 4));
  survey.valid = payload[36];
  survey.active = payload[37];
  if (configuration_state == "awaiting_stop" && now < deadline) {
    // 按GPS周回绕比较epoch；请求前、ACK前及重复epoch不能证明已停止。
    const auto elapsed =
        stop_tow ? (std::uint64_t{tow} + kGpsWeekMs - *stop_tow) % kGpsWeekMs
                 : 0;
    const bool newer = stop_tow && elapsed > 0 && elapsed < kGpsWeekMs / 2;
    if (!stop_tow || newer) {
      stop_tow = tow;
      if (newer && !survey.active) {
        phase = Phase::kSurvey;
        configuration_state = "pending";
      }
    }
  }
}

void BaseStation::Impl::HandleNmea(std::span<const std::uint8_t> sentence) {
  const auto star = std::find(sentence.begin(), sentence.end(), '*');
  if (star == sentence.end() || sentence.end() - star != 5 ||
      sentence[sentence.size() - 2] != '\r' || Hex(star[1]) < 0 ||
      Hex(star[2]) < 0) {
    ++nmea_errors;
    return;
  }
  std::uint8_t checksum = 0;
  for (auto it = sentence.begin() + 1; it != star; ++it) checksum ^= *it;
  if (checksum == ((Hex(star[1]) << 4) | Hex(star[2])))
    ++nmea_frames;
  else
    ++nmea_errors;
}

void BaseStation::Impl::HandleReadback(std::span<const std::uint8_t> payload) {
  if (configuration_state != "awaiting_readback") return;
  if (payload.size() < 4 || payload[0] != 1 || payload[1] != 0 ||
      payload[2] != 0 || payload[3] != 0) {
    FailConfiguration("invalid RAM configuration readback");
    return;
  }
  ConfigValues actual;
  std::size_t offset = 4;
  while (offset < payload.size()) {
    if (payload.size() - offset < 4) break;
    const auto key =
        static_cast<std::uint32_t>(ReadLe(payload.subspan(offset, 4)));
    offset += 4;
    const auto size = KeySize(key);
    if (!size || payload.size() - offset < size) break;
    actual[key] = ReadLe(payload.subspan(offset, size));
    offset += size;
  }
  if (offset != payload.size() || actual != desired) {
    FailConfiguration(
        "RAM configuration readback did not match requested values");
    return;
  }
  configuration_state = "verified";
  phase = Phase::kNone;
  verified_at = now;
  // 配置之前已排队的旧完成帧不属于新一轮，读回后等待新的 NAV-SVIN 证据。
  survey = {};
}

void BaseStation::Impl::AdvanceConfiguration() {
  if (configuration_state != "pending") return;
  const bool readback = phase == Phase::kReadback;
  const auto values =
      phase == Phase::kDisable ? ConfigValues{{kTimeMode, 0}} : desired;
  if (!Send(ConfigurationMessage(values, readback))) return;
  configuration_state = readback ? "awaiting_readback" : "awaiting_ack";
  deadline = now + 1s;
}

bool BaseStation::Impl::Send(const Bytes &bytes) {
  if (port->Write(bytes)) return true;
  Disconnect("base serial write failed");
  return false;
}

void BaseStation::Impl::Disconnect(const std::string &error) {
  if (Busy())
    FailConfiguration(error);
  else
    configuration_state = "idle";
  port.reset();
  incoming.clear();
  discard_prefix = 0;
  pending_rtcm.clear();
  rtcm_queue_dropped = 0;
  any_rx.reset();
  version_rx.reset();
  rtcm_rx.reset();
  verified_at.reset();
  stop_tow.reset();
  partial_since.reset();
  version.clear();
  software.clear();
  firmware.clear();
  hardware.clear();
  module.clear();
  receiver_error.clear();
  survey = {};
  rx_bytes = 0;
  ubx_frames = 0;
  ubx_checksum_errors = 0;
  rtcm_frames = 0;
  rtcm_bytes = 0;
  rtcm_crc_errors = 0;
  nmea_frames = 0;
  nmea_errors = 0;
  rtcm_types.clear();
  framing_errors = 0;
  phase = Phase::kNone;
  desired.clear();
  last_error = error;
  next_open = now + 1s;
}

void BaseStation::Impl::FailConfiguration(const std::string &error) {
  configuration_state = "failed";
  phase = Phase::kNone;
  verified_at.reset();
  stop_tow.reset();
  last_error = error;
}

bool BaseStation::Impl::ReceiverVerified(Time at) const {
  return port && version_rx && receiver_error.empty() &&
         !Stale(version_rx, at, settings.stale_after_ms);
}

bool BaseStation::Impl::Busy() const {
  return configuration_state == "pending" ||
         configuration_state == "awaiting_ack" ||
         configuration_state == "awaiting_stop" ||
         configuration_state == "awaiting_readback";
}

Json BaseStation::Impl::Status(Time at) const {
  const bool known = survey.received_at.has_value();
  const bool survey_stale =
      Stale(survey.sampled_at, at, settings.stale_after_ms);
  const bool current_configuration =
      configuration_state == "idle" || configuration_state == "verified";
  const bool complete = current_configuration && ReceiverVerified(at) &&
                        known && !survey_stale && survey.valid &&
                        !survey.active;
  Json xyz = nullptr;
  if (known && (survey.observations || survey.valid))
    xyz = {{"x", survey.x}, {"y", survey.y}, {"z", survey.z}};
  Json types = Json::object();
  for (const auto &[type, count] : rtcm_types) types[type] = count;
  const auto error = !receiver_error.empty() ? receiver_error : last_error;
  return {
      {"connected", port.has_value()},
      {"receiver_verified", ReceiverVerified(at)},
      {"device", settings.device},
      {"baud", settings.baud},
      {"rx_bytes", rx_bytes},
      {"last_rx_age_ms", Age(any_rx, at)},
      {"protocol",
       {{"name", "UBX"},
        {"version", version.empty() ? Json(nullptr) : Json(version)},
        {"software", software.empty() ? Json(nullptr) : Json(software)},
        {"firmware", firmware.empty() ? Json(nullptr) : Json(firmware)},
        {"hardware", hardware.empty() ? Json(nullptr) : Json(hardware)},
        {"module", module.empty() ? Json(nullptr) : Json(module)},
        {"last_rx_age_ms", Age(version_rx, at)},
        {"stale", Stale(version_rx, at, settings.stale_after_ms)},
        {"valid_frames", ubx_frames},
        {"checksum_errors", ubx_checksum_errors},
        {"framing_errors", framing_errors}}},
      {"survey_in",
       {{"known", known},
        {"stale", survey_stale},
        {"active", known ? Json(survey.active) : Json(nullptr)},
        {"valid", known ? Json(survey.valid) : Json(nullptr)},
        {"completed", complete},
        {"ecef_m", xyz},
        {"duration_s", known ? Json(survey.duration_s) : Json(nullptr)},
        {"mean_accuracy_m", known ? Json(survey.accuracy_m) : Json(nullptr)},
        {"observations", known ? Json(survey.observations) : Json(nullptr)},
        {"i_tow_ms", known ? Json(survey.tow_ms) : Json(nullptr)},
        {"sample_sequence", survey.sequence},
        {"raw_hex",
         survey.raw_hex.empty() ? Json(nullptr) : Json(survey.raw_hex)},
        {"last_rx_age_ms", Age(survey.received_at, at)},
        {"last_sample_age_ms", Age(survey.sampled_at, at)}}},
      {"rtcm",
       {{"valid_frames", rtcm_frames},
        {"activation_barrier", activation_barrier},
        {"queue_dropped", rtcm_queue_dropped},
        {"bytes", rtcm_bytes},
        {"message_types", types},
        {"crc_errors", rtcm_crc_errors},
        {"last_rx_age_ms", Age(rtcm_rx, at)},
        {"stale", Stale(rtcm_rx, at, settings.stale_after_ms)}}},
      {"nmea",
       {{"valid_sentences", nmea_frames}, {"checksum_errors", nmea_errors}}},
      {"configuration",
       {{"state", configuration_state},
        {"storage", "RAM"},
        {"survey_min_duration_s",
         requested_duration ? Json(requested_duration) : Json(nullptr)},
        {"survey_accuracy_m",
         requested_duration ? Json(requested_accuracy) : Json(nullptr)},
        {"verified_age_ms", Age(verified_at, at)}}},
      {"last_error", error.empty() ? Json(nullptr) : Json(error)}};
}

BaseStation::BaseStation(Settings settings)
    : impl_(std::make_unique<Impl>(std::move(settings))) {}
BaseStation::~BaseStation() = default;
void BaseStation::Tick(Time now) { impl_->Tick(now); }
std::expected<void, std::string> BaseStation::StartSurveyIn() {
  return StartSurveyIn(impl_->settings.survey_min_duration_s,
                       impl_->settings.survey_accuracy_m);
}
std::expected<void, std::string> BaseStation::StartSurveyIn(
    std::uint32_t duration_s, double accuracy_m) {
  if (!impl_->ReceiverVerified(impl_->now))
    return std::unexpected(impl_->receiver_error.empty()
                               ? "base receiver is not freshly verified"
                               : impl_->receiver_error);
  if (impl_->Busy())
    return std::unexpected("base configuration is already in progress");
  impl_->requested_duration = duration_s;
  impl_->requested_accuracy = accuracy_m;
  impl_->desired = SurveyConfig(duration_s, accuracy_m);
  impl_->configuration_state = "pending";
  impl_->phase = Impl::Phase::kDisable;
  impl_->verified_at.reset();
  impl_->stop_tow = impl_->survey.sampled_at
                        ? std::optional{impl_->survey.tow_ms}
                        : std::nullopt;
  impl_->last_error.clear();
  impl_->survey = {};
  return {};
}
void BaseStation::CancelPendingConfiguration() {
  if (impl_->Busy()) impl_->FailConfiguration("configuration cancelled");
}
std::vector<RtcmFrame> BaseStation::DrainRtcmFrames() {
  return std::exchange(impl_->pending_rtcm, {});
}
void BaseStation::DiscardRtcmBeforeActivation(Time now) {
  impl_->now = now;
  impl_->activation_barrier = true;
  for (auto& frame : impl_->pending_rtcm) frame.eligible_for_relay = false;
  if (impl_->port) impl_->Receive();
}
Json BaseStation::Status(Time now) const { return impl_->Status(now); }
}  // namespace rtk
