#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

#include "imu/bmi088.hpp"

namespace imu {
using Clock = std::chrono::steady_clock;

struct ReceiveResult {
  bool recognized{};
  std::optional<nlohmann::json> sample;
  std::string_view diagnostic;
};

class Stream {
 public:
  Stream();
  ReceiveResult Receive(const mavlink_message_t& message, Clock::time_point received_at,
                        Clock::time_point now);
  std::optional<nlohmann::json> Snapshot(Clock::time_point now, bool include_status) const;
  void Tick(Clock::time_point now);
  void Reset();

 private:
  using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
  struct TimedPacket { Packet packet; Clock::time_point received_at; };
  struct Pending {
    std::optional<TimedPacket> raw, parsed;
    Clock::time_point created_at;
    bool revoked{};
  };
  struct Pair { TimedPacket raw, parsed; bool revoked{}; };

  ReceiveResult AcceptSample(Packet packet, Clock::time_point received_at, Clock::time_point now);
  nlohmann::json BaseJson() const;
  nlohmann::json SampleJson(const Pair& pair, Clock::time_point now) const;
  nlohmann::json PairJson(const Pair& pair, Clock::time_point now) const;
  nlohmann::json StatusJson(Clock::time_point now) const;
  void Revoke();

  std::optional<std::pair<std::uint8_t, std::uint8_t>> source_;
  std::string epoch_;
  std::optional<Key> frontier_;
  std::map<Key, Pending> pending_;
  std::map<Key, Clock::time_point> retired_;
  std::optional<Pair> latest_;
  std::optional<TimedPacket> status_;
};

nlohmann::json SampleMessage(nlohmann::json imu, const std::string& device_id,
                              std::chrono::system_clock::time_point sent_at);
}  // namespace imu
