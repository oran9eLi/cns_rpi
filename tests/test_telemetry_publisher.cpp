#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "telemetry/publisher.hpp"

using namespace std::chrono_literals;

namespace {

telemetry::ChannelDefinition Channel(std::string name, std::string topic,
                                     std::chrono::milliseconds interval,
                                     bool enabled = true) {
  return {
      {std::move(name), std::move(topic), interval, 0, false, enabled},
      [](const state::TelemetryState&, std::uint64_t sequence) {
        return nlohmann::json{{"sequence", sequence}};
      },
  };
}

}  // namespace

TEST_CASE("到期通道共享一次最新状态快照") {
  const auto start = telemetry::Clock::time_point{};
  telemetry::Publisher publisher(
      {Channel("快照", "snapshot", 1000ms),
       Channel("实时", "realtime", 100ms)},
      start);
  int snapshot_calls = 0;
  std::vector<std::string> topics;
  auto snapshot = [&] {
    ++snapshot_calls;
    return state::TelemetryState{};
  };
  auto publish = [&](const std::string& topic, const std::string&, int, bool) {
    topics.push_back(topic);
    return true;
  };

  CHECK(publisher.Tick(start + 99ms, snapshot, publish).empty());
  CHECK(snapshot_calls == 0);

  const auto fast = publisher.Tick(start + 100ms, snapshot, publish);
  REQUIRE(fast.size() == 1);
  CHECK(fast[0].channel_name == "实时");
  CHECK(snapshot_calls == 1);

  const auto both = publisher.Tick(start + 1000ms, snapshot, publish);
  REQUIRE(both.size() == 2);
  CHECK(snapshot_calls == 2);
  CHECK(topics == std::vector<std::string>{"realtime", "snapshot", "realtime"});
}

TEST_CASE("失败帧不重传且连续失败只提示一次") {
  const auto start = telemetry::Clock::time_point{};
  telemetry::Publisher publisher({Channel("实时", "realtime", 100ms)}, start);
  std::vector<std::uint64_t> sequences;
  bool succeeds = false;
  auto snapshot = [] { return state::TelemetryState{}; };
  auto publish = [&](const std::string&, const std::string& frame, int, bool) {
    sequences.push_back(nlohmann::json::parse(frame).at("sequence").get<std::uint64_t>());
    return succeeds;
  };

  const auto first = publisher.Tick(start + 100ms, snapshot, publish);
  REQUIRE(first.size() == 1);
  CHECK_FALSE(first[0].succeeded);
  CHECK(first[0].should_log_failure);

  const auto repeated = publisher.Tick(start + 200ms, snapshot, publish);
  REQUIRE(repeated.size() == 1);
  CHECK_FALSE(repeated[0].should_log_failure);

  succeeds = true;
  const auto recovered = publisher.Tick(start + 300ms, snapshot, publish);
  REQUIRE(recovered.size() == 1);
  CHECK(recovered[0].succeeded);

  succeeds = false;
  const auto failed_again = publisher.Tick(start + 400ms, snapshot, publish);
  REQUIRE(failed_again.size() == 1);
  CHECK(failed_again[0].should_log_failure);
  CHECK(sequences == std::vector<std::uint64_t>{0, 1, 2, 3});
}

TEST_CASE("禁用通道永不发布且Reset重新开始节拍和序号") {
  const auto start = telemetry::Clock::time_point{};
  telemetry::Publisher publisher(
      {Channel("禁用", "disabled", 100ms, false),
       Channel("实时", "realtime", 100ms)},
      start);
  int snapshot_calls = 0;
  std::vector<std::uint64_t> sequences;
  auto snapshot = [&] {
    ++snapshot_calls;
    return state::TelemetryState{};
  };
  auto publish = [&](const std::string& topic, const std::string& frame, int, bool) {
    CHECK(topic == "realtime");
    sequences.push_back(nlohmann::json::parse(frame).at("sequence").get<std::uint64_t>());
    return false;
  };

  const auto first = publisher.Tick(start + 100ms, snapshot, publish);
  REQUIRE(first.size() == 1);
  CHECK(first[0].should_log_failure);

  publisher.Reset(start + 150ms);
  CHECK(publisher.Tick(start + 150ms, snapshot, publish).empty());
  const auto after_reset = publisher.Tick(start + 250ms, snapshot, publish);
  REQUIRE(after_reset.size() == 1);
  CHECK(after_reset[0].should_log_failure);
  CHECK(snapshot_calls == 2);
  CHECK(sequences == std::vector<std::uint64_t>{0, 0});
}
