#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <regex>

#include "bmi088_fixture.hpp"
#include "imu/stream.hpp"

using namespace bmi_fixture;
using namespace std::chrono_literals;
namespace {
imu::Clock::time_point At(int ms) { return imu::Clock::time_point{} + std::chrono::milliseconds(ms); }
auto Feed(imu::Stream& stream, Bytes bytes, int kind, int received, int now = -1,
          std::uint8_t sys = 218) {
  return stream.Receive(Message(bytes, kind, sys), At(received), At(now < 0 ? received : now));
}
auto Pair(imu::Stream& stream, int received = 5, int now = 20,
          std::uint32_t seq = 42, std::uint32_t time = 10000) {
  auto raw = Payload(0), parsed = Payload(1);
  for (auto* bytes : {&raw, &parsed}) {
    Put32(*bytes, 4, seq); Put32(*bytes, 8, time);
    Put32(*bytes, 12, time + ((*bytes)[1] == 0 ? 5 : 8));
  }
  Feed(stream, raw, 0, received, now);
  return Feed(stream, parsed, 1, received + 3, now);
}
auto Sample(const imu::Stream& stream, int now) { return stream.Snapshot(At(now), false)->at("sample"); }
std::string Epoch(const imu::Stream& stream, int now) {
  return stream.Snapshot(At(now), false)->at("source_epoch").get<std::string>();
}
}

TEST_CASE("full emitted JSON matches the documented contract") {
  imu::Stream stream;
  CHECK_FALSE(stream.Snapshot(At(0), true));
  auto result = Pair(stream);
  REQUIRE(result.sample);
  auto pair = *result.sample;
  REQUIRE(pair.contains("source_epoch"));
  CHECK(std::regex_match(pair["source_epoch"].get<std::string>(),
        std::regex("[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")));
  const auto epoch = Contract()["pair"]["imu"]["source_epoch"];
  pair["source_epoch"] = epoch;
  CHECK(pair == Contract()["pair"]["imu"]);
  auto realtime = *stream.Snapshot(At(20), false);
  realtime["source_epoch"] = epoch;
  CHECK(realtime == Contract()["realtime"]["telemetry"]["imu"]);
  Feed(stream, Payload(2), 2, 20);
  auto snapshot = *stream.Snapshot(At(20), true);
  snapshot["source_epoch"] = epoch;
  CHECK(snapshot == Contract()["snapshot"]["telemetry"]["imu"]);
  auto message = imu::SampleMessage(pair, "DCDWCNS1S2MEAG1VTA0C",
      std::chrono::sys_days(std::chrono::year{2026}/11/12) + 2h + 20ms);
  CHECK(message == Contract()["pair"]);
}

TEST_CASE("freshness advances with no UART input and cannot be renewed by retries") {
  imu::Stream stream;
  REQUIRE(Pair(stream).sample);
  CHECK(Sample(stream, 200)["valid"] == true);
  CHECK(Sample(stream, 200)["valid_for_ms"] == 0);
  CHECK(Sample(stream, 201)["value"].is_null());
  CHECK(Sample(stream, 201)["invalid_reason"] == "sample_stale");
  auto raw = Payload(0); Put32(raw, 12, 10190);
  CHECK_FALSE(Feed(stream, raw, 0, 190).sample);
  CHECK_FALSE(Feed(stream, Payload(1), 1, 190).sample);
  CHECK(Sample(stream, 230)["age_ms"] == 230);
  CHECK_FALSE(Feed(stream, raw, 0, 2000).sample);
  CHECK_FALSE(Feed(stream, Payload(1), 1, 2000).sample);
  CHECK(Sample(stream, 2000)["value"].is_null());
}

TEST_CASE("partial retry preserves first bytes and age; pairs may interleave in either order") {
  imu::Stream stream;
  auto raw = Payload(0);
  CHECK_FALSE(Feed(stream, raw, 0, 5).sample);
  Put32(raw, 12, 10090);
  CHECK_FALSE(Feed(stream, raw, 0, 90).sample);
  auto newer = Payload(1);
  Put32(newer, 4, 43); Put32(newer, 8, 10200); Put32(newer, 12, 10208);
  Feed(stream, newer, 1, 208);
  auto result = Feed(stream, Payload(1), 1, 8, 220);
  REQUIRE(result.sample);
  CHECK((*result.sample)["sample"]["age_ms"] == 220);
  CHECK((*result.sample)["sample"]["value"].is_null());
  CHECK((*result.sample)["raw_payload_hex"] == Contract()["pair"]["imu"]["raw_payload_hex"]);
  raw = Payload(0);
  Put32(raw, 4, 43); Put32(raw, 8, 10200); Put32(raw, 12, 10205);
  REQUIRE(Feed(stream, raw, 0, 205, 220).sample);
  CHECK(Sample(stream, 220)["sample_sequence"] == 43);
  CHECK(Sample(stream, 220)["age_ms"] == 20);
}

TEST_CASE("incomplete expiry, content conflicts and raw mismatch cannot produce a pair") {
  imu::Stream stream;
  Feed(stream, Payload(0), 0, 5);
  stream.Tick(At(1005));
  CHECK_FALSE(Feed(stream, Payload(1), 1, 1006).sample);
  CHECK(stream.Snapshot(At(1006), false)->at("sample").is_null());
  stream.Reset();
  auto raw = Payload(0);
  Feed(stream, raw, 0, 5);
  raw[28] = 1;
  CHECK_FALSE(Feed(stream, raw, 0, 6).diagnostic.empty());
  CHECK_FALSE(Feed(stream, Payload(1), 1, 8).sample);
  for (int offset : {20, 32, 44}) {
    stream.Reset();
    Feed(stream, Payload(0), 0, 5);
    auto parsed = Payload(1); parsed[offset] += 2;
    auto result = Feed(stream, parsed, 1, 8);
    CHECK_FALSE(result.sample);
    CHECK_FALSE(result.diagnostic.empty());
  }
  stream.Reset();
  Feed(stream, Payload(0), 0, 5);
  auto parsed = Payload(1); parsed[3] = 2;
  CHECK_FALSE(Feed(stream, parsed, 1, 8).sample);
}

TEST_CASE("axis mapping is applied once, with one integer unit tolerance") {
  imu::Stream stream;
  auto raw = Payload(0), parsed = Payload(1);
  raw[28] = 0xab; raw[29] = 0x0a; raw[32] = raw[33] = 0;
  raw[39] = 0; raw[40] = 0x40;
  parsed[52] = 2; parsed[53] = 255;
  Put32(parsed, 24, static_cast<std::uint32_t>(-1000)); Put32(parsed, 28, 0);
  Put32(parsed, 36, static_cast<std::uint32_t>(-500000));
  Feed(stream, raw, 0, 5);
  auto result = Feed(stream, parsed, 1, 8);
  REQUIRE(result.sample);
  CHECK((*result.sample)["sample"]["value"]["accel_mg"] == nlohmann::json::array({0,-1000,0}));
  stream.Reset();
  parsed = Payload(1); Put32(parsed, 28, 1001);
  Feed(stream, Payload(0), 0, 5);
  REQUIRE(Feed(stream, parsed, 1, 8).sample);
}

TEST_CASE("STATUS withdrawal revokes existing and incomplete keys; a new key may recover") {
  imu::Stream stream;
  REQUIRE(Pair(stream).sample);
  auto status = Payload(2); status[61] = 0; status[2] = 9;
  Put32(status, 16, 2);
  Feed(stream, status, 2, 21);
  CHECK(Sample(stream, 21)["invalid_reason"] == "source_unavailable");
  CHECK(Sample(stream, 21)["reinit_count"] == 1);
  CHECK_FALSE(Feed(stream, Payload(0), 0, 22).sample);
  CHECK_FALSE(Feed(stream, Payload(1), 1, 22).sample);
  status[61] = 1; status[2] = 3;
  Feed(stream, status, 2, 23);
  CHECK(Sample(stream, 23)["value"].is_null());
  REQUIRE(Pair(stream, 205, 220, 43, 10200).sample);
  CHECK(Sample(stream, 220)["valid"] == true);
  stream.Reset();
  Feed(stream, Payload(0), 0, 5);
  status[61] = 0; status[2] = 9;
  Feed(stream, status, 2, 6);
  auto result = Feed(stream, Payload(1), 1, 8);
  REQUIRE(result.sample);
  CHECK((*result.sample)["sample"]["value"].is_null());
}

TEST_CASE("unknown age, wrap-safe encoding age, and zero sample time") {
  imu::Stream stream;
  auto raw = Payload(0), parsed = Payload(1);
  raw[2] = parsed[2] = 5;
  Put32(raw, 12, 9999); Put32(parsed, 12, 9999);
  Feed(stream, raw, 0, 5);
  auto result = Feed(stream, parsed, 1, 8);
  REQUIRE(result.sample);
  CHECK((*result.sample)["sample"]["invalid_reason"] == "age_unknown");
  CHECK((*result.sample)["sample"]["age_ms"].is_null());
  stream.Reset();
  REQUIRE(Pair(stream, 5, 20, 1, 0).sample);
  CHECK(Sample(stream, 20)["sample_time_ms"] == 0);
  stream.Reset();
  REQUIRE(Pair(stream, 5, 20, 1, 0xfffffffc).sample);
  CHECK(Sample(stream, 20)["age_ms"] == 20);
}

TEST_CASE("an invalidated duplicate revokes value even after the pair was emitted") {
  imu::Stream stream;
  REQUIRE(Pair(stream).sample);
  auto revoked = Payload(0); revoked[2] = 9;
  CHECK_FALSE(Feed(stream, revoked, 0, 21).sample);
  CHECK(Sample(stream, 21)["invalid_reason"] == "source_unavailable");
  CHECK(Sample(stream, 21)["value"].is_null());
}

TEST_CASE("the last emitted key stays retired when a newer incomplete pair expires") {
  imu::Stream stream;
  REQUIRE(Pair(stream).sample);
  const auto epoch = Epoch(stream, 20);
  auto newer = Payload(0);
  Put32(newer, 4, 43); Put32(newer, 8, 10200); Put32(newer, 12, 10205);
  Feed(stream, newer, 0, 205);
  stream.Tick(At(1205));
  CHECK_FALSE(Feed(stream, Payload(0), 0, 1300).sample);
  CHECK_FALSE(Feed(stream, Payload(1), 1, 1303).sample);
  CHECK(Epoch(stream, 1303) == epoch);
  CHECK(Sample(stream, 1303)["value"].is_null());
}

TEST_CASE("STATUS only, unknown diagnostics, SPI addresses and received timeout") {
  imu::Stream stream;
  Feed(stream, Payload(2), 2, 0);
  REQUIRE(stream.Snapshot(At(0), true));
  CHECK(Sample(stream, 0).is_null());
  CHECK(stream.Snapshot(At(3000), true)->at("status")["stale"] == false);
  CHECK(stream.Snapshot(At(3001), true)->at("status")["stale"] == true);
  auto status = Payload(2);
  status[2] = 0; status[3] = 0; status[60] = 0; status[61] = 0; status[62] = 255;
  Put32(status, 56, 0xffffffff);
  Feed(stream, status, 2, 4000);
  auto json = stream.Snapshot(At(4000), true)->at("status");
  for (auto field : {"bus", "sample_sequence", "sample_time_ms", "source_age_ms", "device_ready",
      "reinit_count", "accel_chip_id", "gyro_chip_id", "accel_addr_7bit", "gyro_addr_7bit",
      "driver_stage", "driver_result", "bsp_result", "driver_error_count", "fifo_drop_count",
      "framework_state"}) CHECK(json[field].is_null());
  CHECK(json["source_overwrites"] == 39);
  status[3] = 2; status[60] = 1; status[54] = status[55] = 0;
  status[52] = 0xab;
  Feed(stream, status, 2, 4001);
  json = stream.Snapshot(At(4001), true)->at("status");
  CHECK(json["accel_addr_7bit"].is_null());
  CHECK(json["accel_chip_id"] == 0xab);
}

TEST_CASE("source reset changes epoch and clears halves; Comm counters do not reset it") {
  imu::Stream stream;
  REQUIRE(Pair(stream).sample);
  auto epoch = Epoch(stream, 20);
  auto status = Payload(2); Put32(status, 36, 0);
  Feed(stream, status, 2, 21);
  CHECK(Epoch(stream, 21) == epoch);
  auto raw = Payload(0); Put32(raw, 4, 1); Put32(raw, 8, 1); Put32(raw, 12, 5);
  Feed(stream, raw, 0, 25);
  CHECK(Epoch(stream, 25) != epoch);
  CHECK(Sample(stream, 25).is_null());
  epoch = Epoch(stream, 25);
  Feed(stream, Payload(1), 1, 26, 26, 219);
  CHECK(Epoch(stream, 26) != epoch);
  CHECK(Sample(stream, 26).is_null());
  stream.Reset();
  CHECK_FALSE(stream.Snapshot(At(27), true));
}

TEST_CASE("first STATUS clock regression clears both last sample and incomplete pairs") {
  for (bool complete : {false, true}) {
    imu::Stream stream;
    if (complete) REQUIRE(Pair(stream).sample);
    else Feed(stream, Payload(0), 0, 5);
    const auto epoch = Epoch(stream, 20);
    auto rebooted = Payload(2);
    Put32(rebooted, 4, 1); Put32(rebooted, 8, 80);
    Put32(rebooted, 12, 100); Put32(rebooted, 56, 20);
    Feed(stream, rebooted, 2, 21);
    CHECK(Epoch(stream, 21) != epoch);
    CHECK(Sample(stream, 21).is_null());
    CHECK_FALSE(Feed(stream, Payload(1), 1, 25).sample);
  }
}

TEST_CASE("a low-uptime measurement after STATUS starts a new source without old diagnostics") {
  imu::Stream stream;
  Feed(stream, Payload(2), 2, 0);
  const auto epoch = Epoch(stream, 0);
  REQUIRE(Pair(stream, 5, 20, 1, 80).sample);
  CHECK(Epoch(stream, 20) != epoch);
  CHECK(stream.Snapshot(At(20), true)->at("status").is_null());

  stream.Reset();
  auto no_sample = Payload(2);
  no_sample[2] = 0; no_sample[61] = 0;
  Put32(no_sample, 4, 0); Put32(no_sample, 8, 0); Put32(no_sample, 56, 0xffffffff);
  Feed(stream, no_sample, 2, 0);
  const auto empty_epoch = Epoch(stream, 0);
  REQUIRE(Pair(stream, 5, 20, 1, 80).sample);
  CHECK(Epoch(stream, 20) != empty_epoch);
  CHECK(stream.Snapshot(At(20), true)->at("status").is_null());
}

TEST_CASE("small cross-kind encode reordering retains epoch and valid STATUS diagnostics") {
  imu::Stream stream;
  Feed(stream, Payload(2), 2, 0);
  const auto epoch = Epoch(stream, 0);
  REQUIRE(Pair(stream).sample);
  CHECK(Epoch(stream, 20) == epoch);
  CHECK(stream.Snapshot(At(20), true)->at("status")["device_ready"] == true);
  CHECK(Sample(stream, 20)["valid"] == true);
}
