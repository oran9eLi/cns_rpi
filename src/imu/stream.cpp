#include "imu/stream.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

namespace imu {
namespace {
using namespace std::chrono_literals;
std::string NewEpoch();
std::int64_t Elapsed(Clock::time_point now, Clock::time_point then);
std::optional<std::int64_t> Age(const Packet& packet, Clock::time_point received, Clock::time_point now);
bool SameContent(const Packet& first, const Packet& retry);
bool Consistent(const Packet& raw, const Packet& parsed);
std::string Hex(const std::array<std::uint8_t, 64>& payload);
}  // namespace

Stream::Stream() { Reset(); }

ReceiveResult Stream::Receive(const mavlink_message_t& message, Clock::time_point received_at,
                               Clock::time_point now) {
  auto decoded = Decode(message);
  if (!decoded) return {false, {}, decoded.error()};
  if (!*decoded) return {};
  auto packet = std::move(**decoded);
  Tick(now);
  const auto source = std::pair{packet.system_id, packet.component_id};
  if (source_ && *source_ != source) Reset();
  source_ = source;
  if (packet.header.kind != 2) return AcceptSample(std::move(packet), received_at, now);

  const bool status_clock_regressed =
      (status_ && packet.header.encode_time < status_->packet.header.encode_time) ||
      (frontier_ && packet.header.encode_time < std::get<2>(*frontier_));
  if (status_clock_regressed) {
    Reset();
    source_ = source;
  }
  const auto& status = std::get<Status>(packet.data);
  if (!status.source_available || packet.header.Invalidated()) Revoke();
  status_ = TimedPacket{std::move(packet), received_at};
  return {true, {}, {}};
}

std::optional<nlohmann::json> Stream::Snapshot(Clock::time_point now, bool include_status) const {
  if (!source_) return std::nullopt;
  auto result = BaseJson();
  result["sample"] = latest_ ? SampleJson(*latest_, now) : nlohmann::json(nullptr);
  if (include_status) result["status"] = status_ ? StatusJson(now) : nlohmann::json(nullptr);
  return result;
}

void Stream::Tick(Clock::time_point now) {
  std::erase_if(retired_, [&](const auto& item) { return now - item.second >= 1s; });
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (now - it->second.created_at >= 1s) {
      retired_[it->first] = now;
      it = pending_.erase(it);
    } else ++it;
  }
}

void Stream::Reset() {
  source_.reset();
  epoch_ = NewEpoch();
  frontier_.reset();
  pending_.clear();
  retired_.clear();
  latest_.reset();
  status_.reset();
}

ReceiveResult Stream::AcceptSample(Packet packet, Clock::time_point received_at,
                                    Clock::time_point now) {
  const auto& header = packet.header;
  const Key key{header.reinit_count, header.sequence, header.sample_time};
  if (header.Invalidated()) Revoke();
  const bool already_emitted = latest_ && key == Key{latest_->raw.packet.header.reinit_count,
      latest_->raw.packet.header.sequence, latest_->raw.packet.header.sample_time};
  if (retired_.contains(key) ||
      already_emitted || (frontier_ == key && !pending_.contains(key))) return {true, {}, {}};
  if (!pending_.contains(key)) {
    // Small cross-kind encode reordering is normal within the 200ms frozen-pair window.
    const bool clock_regressed_since_status = status_ &&
        static_cast<std::uint64_t>(header.encode_time) + 200 < status_->packet.header.encode_time;
    if (clock_regressed_since_status ||
        (frontier_ && (header.reinit_count != std::get<0>(*frontier_) ||
                       header.sequence < std::get<1>(*frontier_) ||
                       header.sample_time < std::get<2>(*frontier_)))) {
      const auto source = source_;
      Reset();
      source_ = source;
    }
    frontier_ = key;
  }
  if (now - received_at >= 1s) {
    retired_[key] = now;
    return {true, {}, "expired BMI088 half-pair"};
  }
  auto& pending = pending_.try_emplace(key, Pending{{}, {}, received_at, false}).first->second;
  auto& slot = header.kind == 0 ? pending.raw : pending.parsed;
  if (slot) {
    if (SameContent(slot->packet, packet)) return {true, {}, {}};
    pending_.erase(key);
    retired_[key] = now;
    return {true, {}, "conflicting BMI088 retry content"};
  }
  pending.revoked = pending.revoked || header.Invalidated();
  slot = TimedPacket{std::move(packet), received_at};
  if (!pending.raw || !pending.parsed) return {true, {}, {}};
  Pair pair{std::move(*pending.raw), std::move(*pending.parsed), pending.revoked};
  pending_.erase(key);
  retired_[key] = now;
  if (!Consistent(pair.raw.packet, pair.parsed.packet))
    return {true, {}, "BMI088 RAW/PARSED bus or engineering-value mismatch"};
  // An earlier interleaved pair is still observable, but cannot replace a newer current sample.
  if (!latest_ || key > Key{latest_->raw.packet.header.reinit_count,
                            latest_->raw.packet.header.sequence,
                            latest_->raw.packet.header.sample_time}) latest_ = pair;
  return {true, PairJson(pair, now), {}};
}

nlohmann::json Stream::BaseJson() const {
  return {{"sensor", "BMI088"}, {"source_epoch", epoch_},
          {"source", {{"system_id", source_->first}, {"component_id", source_->second}}}};
}

nlohmann::json Stream::SampleJson(const Pair& pair, Clock::time_point now) const {
  const auto& raw_header = pair.raw.packet.header;
  const auto& parsed_header = pair.parsed.packet.header;
  const auto& value = std::get<Parsed>(pair.parsed.packet.data);
  const auto raw_age = Age(pair.raw.packet, pair.raw.received_at, now);
  const auto parsed_age = Age(pair.parsed.packet, pair.parsed.received_at, now);
  std::optional<std::int64_t> age;
  if (raw_age && parsed_age) age = std::max(*raw_age, *parsed_age);
  std::string_view reason;
  if (pair.revoked || raw_header.Invalidated() || parsed_header.Invalidated())
    reason = "source_unavailable";
  else if (!age) reason = "age_unknown";
  else if (*age > 200 || raw_header.Stale() || parsed_header.Stale()) reason = "sample_stale";
  else if (!raw_header.Usable() || !parsed_header.Usable()) reason = "source_unavailable";
  const bool valid = reason.empty();
  std::int64_t remaining = 0;
  if (age) remaining = std::max<std::int64_t>(0, 200 - *age);
  nlohmann::json result{
      {"sample_sequence", raw_header.sequence}, {"sample_time_ms", raw_header.sample_time},
      {"reinit_count", raw_header.reinit_count}, {"raw_encode_time_ms", raw_header.encode_time},
      {"parsed_encode_time_ms", parsed_header.encode_time}, {"bus", raw_header.bus == 1 ? "i2c" : "spi"},
      {"axis_map", value.axis_map}, {"axis_frame", "body"}, {"valid", valid},
      {"invalid_reason", valid ? nlohmann::json(nullptr) : nlohmann::json(reason)},
      {"age_ms", age ? nlohmann::json(*age) : nlohmann::json(nullptr)},
      {"valid_for_ms", valid ? remaining : 0}, {"value", nullptr}};
  if (valid) result["value"] = {
      {"accel_mg", value.accel_mg}, {"gyro_mdps", value.gyro_mdps},
      {"temperature_cdeg", value.temperature_cdeg}, {"sample_period_us", value.sample_period_us},
      {"accel_fullscale_mg", value.accel_fullscale_mg}, {"gyro_fullscale_dps", value.gyro_fullscale_dps},
      {"fifo_drop_count", value.fifo_drop_count}, {"driver_error_count", value.driver_error_count}};
  return result;
}

nlohmann::json Stream::PairJson(const Pair& pair, Clock::time_point now) const {
  auto result = BaseJson();
  result["sample"] = SampleJson(pair, now);
  result["raw_payload_hex"] = Hex(pair.raw.packet.payload);
  result["parsed_payload_hex"] = Hex(pair.parsed.packet.payload);
  return result;
}

nlohmann::json Stream::StatusJson(Clock::time_point now) const {
  const auto& header = status_->packet.header;
  const auto& s = std::get<Status>(status_->packet.data);
  const auto known = [](bool available, auto value) {
    return available ? nlohmann::json(value) : nlohmann::json(nullptr);
  };
  const auto age = Elapsed(now, status_->received_at);
  const bool diag = s.diagnostics_available;
  return {
      {"received_age_ms", age}, {"stale", age > 3000},
      {"flags", {{"present", header.Present()}, {"usable", header.Usable()},
                 {"stale", header.Stale()}, {"invalidated", header.Invalidated()}}},
      {"bus", known(header.bus != 0, header.bus == 1 ? "i2c" : "spi")},
      {"sample_sequence", known(header.Present(), header.sequence)},
      {"sample_time_ms", known(header.Present(), header.sample_time)},
      {"encode_time_ms", header.encode_time}, {"reinit_count", known(diag, header.reinit_count)},
      {"source_age_ms", known(header.Present() && s.source_age_ms != 0xffffffff, s.source_age_ms)},
      {"diagnostics_available", diag}, {"source_available", s.source_available},
      {"device_ready", known(diag, s.device_ready != 0)},
      {"framework_state", known(s.framework_state != 255, s.framework_state)},
      {"accel_chip_id", known(diag, s.accel_chip_id)}, {"gyro_chip_id", known(diag, s.gyro_chip_id)},
      {"accel_addr_7bit", known(diag && header.bus == 1, s.accel_addr)},
      {"gyro_addr_7bit", known(diag && header.bus == 1, s.gyro_addr)},
      {"driver_stage", known(diag, s.driver_stage)}, {"driver_result", known(diag, s.driver_result)},
      {"bsp_result", known(diag, s.bsp_result)}, {"driver_error_count", known(diag, s.driver_error_count)},
      {"fifo_drop_count", known(diag, s.fifo_drop_count)},
      {"source_published_count", s.source_published_count}, {"source_overwrites", s.source_overwrites},
      {"completed_pairs", s.completed_pairs}, {"abandoned_pairs", s.abandoned_pairs},
      {"tx_failures", s.tx_failures}};
}

void Stream::Revoke() {
  if (latest_) latest_->revoked = true;
  for (auto& [key, pending] : pending_) pending.revoked = true;
}

nlohmann::json SampleMessage(nlohmann::json imu, const std::string& device_id,
                              std::chrono::system_clock::time_point sent_at) {
  const auto seconds = std::chrono::floor<std::chrono::seconds>(sent_at);
  const auto time = std::chrono::system_clock::to_time_t(seconds);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream stamp;
  stamp << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
        << std::chrono::duration_cast<std::chrono::milliseconds>(sent_at - seconds).count() << 'Z';
  return {{"schema_version", 1}, {"device_id", device_id}, {"sent_at", stamp.str()}, {"imu", std::move(imu)}};
}

namespace {
std::string NewEpoch() {
  std::random_device random;
  std::array<std::uint8_t, 16> bytes{};
  for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
    result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15];
  }
  return result;
}
std::int64_t Elapsed(Clock::time_point now, Clock::time_point then) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(now - then).count();
}
std::optional<std::int64_t> Age(const Packet& packet, Clock::time_point received, Clock::time_point now) {
  const std::uint32_t encoded_age = packet.header.encode_time - packet.header.sample_time;
  if (encoded_age > 0x7fffffff) return std::nullopt;
  return encoded_age + Elapsed(now, received);
}
bool SameContent(const Packet& first, const Packet& retry) {
  for (std::size_t i = 0; i < 64; ++i) {
    if (i >= 12 && i < 16) continue;  // A UART retry only re-encodes the timestamp.
    if (first.payload[i] != retry.payload[i]) return false;
  }
  return true;
}
bool Consistent(const Packet& raw_packet, const Packet& parsed_packet) {
  if (raw_packet.header.bus != parsed_packet.header.bus) return false;
  const auto& raw = std::get<Raw>(raw_packet.data);
  const auto& parsed = std::get<Parsed>(parsed_packet.data);
  for (int i = 0; i < 3; ++i) {
    const int axis = std::abs(parsed.axis_map[i]) - 1;
    const int sign = parsed.axis_map[i] < 0 ? -1 : 1;
    if (std::abs(raw.accel[axis] * (12000.0 / 32768) * sign - parsed.accel_mg[i]) > 1 ||
        std::abs(raw.gyro[axis] * (1000000.0 / 32768) * sign - parsed.gyro_mdps[i]) > 1) return false;
  }
  return std::abs(raw.temperature_cdeg - parsed.temperature_cdeg) <= 1;
}
std::string Hex(const std::array<std::uint8_t, 64>& payload) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(128);
  for (auto byte : payload) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}
}  // namespace
}  // namespace imu
