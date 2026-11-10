/**
 * @file test_rtk_base_station.cpp
 * @brief 通过真实 PTY 测基站串口、UBX 证据与配置握手，不复制被测解析器。
 */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "rtk/base_station.hpp"

#ifndef SOURCE_DIR
#define SOURCE_DIR "."
#endif

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct Pty {
  int master{::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK)};
  std::string path;
  Pty() {
    REQUIRE(master >= 0);
    REQUIRE(::grantpt(master) == 0);
    REQUIRE(::unlockpt(master) == 0);
    path = ::ptsname(master);
  }
  ~Pty() {
    if (master >= 0) ::close(master);
  }
  Pty(const Pty &) = delete;
  Pty &operator=(const Pty &) = delete;
  void Send(std::span<const std::uint8_t> bytes) const {
    REQUIRE(::write(master, bytes.data(), bytes.size()) ==
            static_cast<ssize_t>(bytes.size()));
  }
  std::vector<std::uint8_t> Drain() const {
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 4096> buffer{};
    for (ssize_t n; (n = ::read(master, buffer.data(), buffer.size())) > 0;)
      bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + n);
    return bytes;
  }
};

std::vector<std::uint8_t> Ubx(std::uint8_t cls, std::uint8_t id,
                              const std::vector<std::uint8_t> &payload) {
  std::vector<std::uint8_t> frame{
      0xb5,
      0x62,
      cls,
      id,
      static_cast<std::uint8_t>(payload.size()),
      static_cast<std::uint8_t>(payload.size() >> 8)};
  frame.insert(frame.end(), payload.begin(), payload.end());
  std::uint8_t a = 0, b = 0;
  for (std::size_t i = 2; i < frame.size(); ++i) {
    a += frame[i];
    b += a;
  }
  frame.push_back(a);
  frame.push_back(b);
  return frame;
}

std::vector<std::uint8_t> MonVer(const std::string &version = "27.31",
                                 const std::string &module = "ZED-F9P") {
  std::vector<std::uint8_t> payload(100, 0);
  const std::string sw = "HPG 1.32", hw = "00190000";
  std::copy(sw.begin(), sw.end(), payload.begin());
  std::copy(hw.begin(), hw.end(), payload.begin() + 30);
  const auto protocol = "PROTVER=" + version, model = "MOD=" + module;
  std::copy(protocol.begin(), protocol.end(), payload.begin() + 40);
  std::copy(model.begin(), model.end(), payload.begin() + 70);
  return Ubx(0x0a, 0x04, payload);
}

void Put32(std::vector<std::uint8_t> &bytes, std::size_t offset,
           std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = value >> (8 * i);
}

std::vector<std::uint8_t> Survey(bool valid, bool active,
                                 std::uint32_t tow = 123000,
                                 std::uint32_t duration_s = 61) {
  std::vector<std::uint8_t> payload(40, 0);
  Put32(payload, 4, tow);
  Put32(payload, 8, duration_s);
  Put32(payload, 12, static_cast<std::uint32_t>(-123456789));
  Put32(payload, 16, 456789012);
  Put32(payload, 20, 345678901);
  payload[24] = static_cast<std::uint8_t>(-9);
  payload[25] = 99;
  payload[26] = 10;
  Put32(payload, 28, 12345);
  Put32(payload, 32, 60);
  payload[36] = valid;
  payload[37] = active;
  return Ubx(0x01, 0x3b, payload);
}

// CRC24Q 手工核对后的固定封装样本；测试只判断帧封装，不声称是完整1005业务负载。
const std::vector<std::uint8_t> kRtcm1005{0xd3, 0,    2,    0x3e,
                                          0xd0, 0xa4, 0xe0, 0};

void VerifyReceiver(Pty &pty, rtk::BaseStation &base, Clock::time_point now) {
  base.Tick(now);
  pty.Drain();
  pty.Send(MonVer());
  base.Tick(now + 1ms);
  REQUIRE(base.Status(now + 1ms)["receiver_verified"].get<bool>());
}

TEST_CASE("activation preserves UBX ACK and NMEA while excluding crossing RTCM") {
  Pty pty;
  rtk::BaseStation base({pty.path});
  const auto now = Clock::now();
  VerifyReceiver(pty, base, now);
  REQUIRE(base.StartSurveyIn());
  base.Tick(now + 2ms); pty.Drain();
  pty.Send(Ubx(0x05, 0x01, {0x06, 0x8a}));
  const std::string nmea = "$GPGLL,4916.45,N,12311.12,W,225444,A,*1D\r\n";
  pty.Send(std::span(reinterpret_cast<const std::uint8_t*>(nmea.data()), nmea.size()));
  pty.Send(std::span(kRtcm1005).first(4));
  base.DiscardRtcmBeforeActivation(now + 3ms);
  auto status = base.Status(now + 3ms);
  CHECK(status["configuration"]["state"] == "awaiting_stop");
  CHECK(status["nmea"]["valid_sentences"] == 1);
  CHECK(status["rtcm"]["activation_barrier"] == false);
  pty.Send(std::span(kRtcm1005).subspan(4));
  base.Tick(now + 4ms);
  const auto crossing = base.DrainRtcmFrames();
  REQUIRE(crossing.size() == 1);
  CHECK_FALSE(crossing.front().eligible_for_relay);
  CHECK(crossing.front().bytes == kRtcm1005);
  pty.Send(kRtcm1005);
  base.Tick(now + 5ms);
  const auto fresh = base.DrainRtcmFrames();
  REQUIRE(fresh.size() == 1);
  CHECK(fresh.front().eligible_for_relay);
  CHECK(fresh.front().bytes == kRtcm1005);
}

const std::vector<std::uint8_t> kSurveyConfig{
    0, 1,    0,    0,   1,    0, 0x77, 0x10, 1, 1,    0,    0x78, 0x10, 1, 4,
    0, 0x78, 0x10, 1,   1,    0, 3,    0x20, 1, 0x8b, 0,    0x91, 0x20, 1, 0xc0,
    2, 0x91, 0x20, 1,   6,    3, 0x91, 0x20, 5, 0x61, 3,    0x91, 0x20, 1, 0x66,
    3, 0x91, 0x20, 1,   0x6b, 3, 0x91, 0x20, 1, 0x70, 3,    0x91, 0x20, 1, 0x10,
    0, 3,    0x40, 120, 0,    0, 0,    0x11, 0, 3,    0x40, 0xd4, 0x30, 0, 0};

const std::vector<std::uint8_t> kReadKeys{
    0,    0, 0,    0,    1,    0, 0x77, 0x10, 1,    0, 0x78, 0x10,
    4,    0, 0x78, 0x10, 1,    0, 3,    0x20, 0x8b, 0, 0x91, 0x20,
    0xc0, 2, 0x91, 0x20, 6,    3, 0x91, 0x20, 0x61, 3, 0x91, 0x20,
    0x66, 3, 0x91, 0x20, 0x6b, 3, 0x91, 0x20, 0x70, 3, 0x91, 0x20,
    0x10, 0, 3,    0x40, 0x11, 0, 3,    0x40};

void ToReadback(Pty &pty, rtk::BaseStation &base, Clock::time_point now) {
  REQUIRE(base.StartSurveyIn(120, 1.25).has_value());
  base.Tick(now + 2ms);
  CHECK(pty.Drain() == Ubx(0x06, 0x8a, {0, 1, 0, 0, 1, 0, 3, 0x20, 0}));
  pty.Send(Ubx(0x05, 0x01, {0x06, 0x8a}));
  pty.Send(Survey(false, false, 123000, 0));
  pty.Send(Survey(false, false, 124000, 0));
  base.Tick(now + 3ms);
  CHECK(pty.Drain() == Ubx(0x06, 0x8a, kSurveyConfig));
  pty.Send(Ubx(0x05, 0x01, {0x06, 0x8a}));
  base.Tick(now + 4ms);
  CHECK(pty.Drain() == Ubx(0x06, 0x8b, kReadKeys));
  CHECK(base.Status(now + 4ms)["configuration"]["state"] ==
        "awaiting_readback");
}

std::vector<std::uint8_t> RecordedMonVer() {
  std::ifstream source(std::filesystem::path(SOURCE_DIR) /
                       "tests/fixtures/rtk/9ps_mon_ver_hpg_1_50.hex");
  REQUIRE(source.good());
  std::string hex;
  source >> hex;
  REQUIRE(hex.size() == 456);
  std::vector<std::uint8_t> bytes;
  for (std::size_t i = 0; i < hex.size(); i += 2)
    bytes.push_back(std::stoul(hex.substr(i, 2), nullptr, 16));
  return bytes;
}

TEST_CASE(
    "opening a USB endpoint does not verify a receiver or complete survey-in") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  CHECK_FALSE(base.Status(now)["connected"].get<bool>());
  base.Tick(now);
  const auto status = base.Status(now);
  CHECK(status["connected"].get<bool>());
  CHECK_FALSE(status["receiver_verified"].get<bool>());
  CHECK(status["survey_in"]["valid"].is_null());
  CHECK(status["rtcm"]["last_rx_age_ms"].is_null());
  CHECK_FALSE(base.StartSurveyIn().has_value());
  CHECK(pty.Drain() == std::vector<std::uint8_t>{0xb5, 0x62, 0x0a, 0x04, 0, 0,
                                                 0x0e, 0x34, 0xb5, 0x62, 0x01,
                                                 0x3b, 0, 0, 0x3c, 0xb5});
}

TEST_CASE(
    "mixed and split UBX NMEA RTCM frames preserve real survey evidence") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  base.Tick(now);
  pty.Drain();
  auto ver = MonVer();
  pty.Send(std::span(ver).first(17));
  base.Tick(now + 1ms);
  CHECK_FALSE(base.Status(now + 1ms)["receiver_verified"].get<bool>());
  pty.Send(std::span(ver).subspan(17));
  const std::string nmea =
      "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
  pty.Send({reinterpret_cast<const std::uint8_t *>(nmea.data()), nmea.size()});
  pty.Send(kRtcm1005);
  pty.Send(Survey(false, true));
  base.Tick(now + 2ms);
  const auto status = base.Status(now + 12ms);
  CHECK(status["receiver_verified"].get<bool>());
  CHECK(status["protocol"]["version"] == "27.31");
  CHECK(status["protocol"]["module"] == "ZED-F9P");
  CHECK(status["survey_in"]["known"].get<bool>());
  CHECK(status["survey_in"]["active"].get<bool>());
  CHECK_FALSE(status["survey_in"]["valid"].get<bool>());
  CHECK_FALSE(status["survey_in"]["completed"].get<bool>());
  CHECK(status["survey_in"]["duration_s"] == 61);
  CHECK(status["survey_in"]["mean_accuracy_m"].get<double>() ==
        doctest::Approx(1.2345));
  CHECK(status["survey_in"]["ecef_m"]["x"].get<double>() ==
        doctest::Approx(-1234567.8909));
  CHECK(status["survey_in"]["ecef_m"]["y"].get<double>() ==
        doctest::Approx(4567890.1299));
  CHECK(status["survey_in"]["ecef_m"]["z"].get<double>() ==
        doctest::Approx(3456789.011));
  CHECK(status["survey_in"]["last_sample_age_ms"] == 10);
  CHECK(status["rtcm"]["valid_frames"] == 1);
  CHECK(status["rtcm"]["bytes"] == 8);
  CHECK(status["rtcm"]["message_types"]["1005"] == 1);
  CHECK(status["rtcm"]["last_rx_age_ms"] == 10);
  CHECK(status["nmea"]["valid_sentences"] == 1);
}

TEST_CASE(
    "checksum errors do not become receiver or RTCM evidence and parser "
    "recovers") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  base.Tick(now);
  pty.Drain();
  auto bad_ver = MonVer();
  bad_ver.back() ^= 1;
  pty.Send(bad_ver);
  auto bad_rtcm = kRtcm1005;
  bad_rtcm.back() ^= 1;
  pty.Send(bad_rtcm);
  base.Tick(now + 1ms);
  auto status = base.Status(now + 1ms);
  CHECK_FALSE(status["receiver_verified"].get<bool>());
  CHECK(status["rtcm"]["valid_frames"] == 0);
  CHECK(status["rtcm"]["last_rx_age_ms"].is_null());
  CHECK(status["rtcm"]["crc_errors"] == 1);
  CHECK(status["protocol"]["checksum_errors"] == 1);
  pty.Send(MonVer());
  pty.Send(kRtcm1005);
  base.Tick(now + 2ms);
  status = base.Status(now + 2ms);
  CHECK(status["receiver_verified"].get<bool>());
  CHECK(status["rtcm"]["valid_frames"] == 1);
}

TEST_CASE(
    "survey completion is a fresh NAV-SVIN result and repeated iTOW does not "
    "refresh sample") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path, .stale_after_ms = 1000});
  const auto now = Clock::now();
  VerifyReceiver(pty, base, now);
  pty.Send(Survey(true, false));
  pty.Send(kRtcm1005);
  base.Tick(now + 2ms);
  CHECK(base.Status(now + 2ms)["survey_in"]["completed"].get<bool>());
  pty.Send(Survey(true, false));
  base.Tick(now + 800ms);
  auto status = base.Status(now + 1100ms);
  CHECK(status["survey_in"]["stale"].get<bool>());
  CHECK_FALSE(status["survey_in"]["completed"].get<bool>());
  CHECK(status["survey_in"]["last_rx_age_ms"] == 300);
  CHECK(status["survey_in"]["last_sample_age_ms"] == 1098);
  CHECK(status["survey_in"]["sample_sequence"] == 1);
  CHECK(status["rtcm"]["stale"].get<bool>());
  CHECK_FALSE(status["receiver_verified"].get<bool>());
  base.Tick(now + 1100ms);
  CHECK_FALSE(base.StartSurveyIn().has_value());
}

TEST_CASE(
    "unknown modules and unsupported protocol versions remain read-only") {
  for (const auto &frame :
       {MonVer("27.31", "NEO-M9N"), MonVer("99.99"), MonVer("")}) {
    Pty pty;
    rtk::BaseStation base({.device = pty.path});
    const auto now = Clock::now();
    base.Tick(now);
    pty.Drain();
    pty.Send(frame);
    base.Tick(now + 1ms);
    CHECK_FALSE(base.Status(now + 1ms)["receiver_verified"].get<bool>());
    CHECK_FALSE(base.StartSurveyIn().has_value());
    CHECK_FALSE(base.Status(now + 1ms)["last_error"].is_null());
    CHECK(pty.Drain().empty());
  }
}

TEST_CASE("idle Tick does not block on the serial VTIME wait") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  base.Tick(now);
  pty.Drain();
  const auto before = Clock::now();
  base.Tick(now + 1ms);
  CHECK(Clock::now() - before < 50ms);
}

TEST_CASE("disconnect clears all current session evidence") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  VerifyReceiver(pty, base, now);
  pty.Send(Survey(true, false));
  pty.Send(kRtcm1005);
  base.Tick(now + 2ms);
  ::close(pty.master);
  pty.master = -1;
  base.Tick(now + 3ms);
  const auto status = base.Status(now + 3ms);
  CHECK_FALSE(status["connected"].get<bool>());
  CHECK_FALSE(status["receiver_verified"].get<bool>());
  CHECK_FALSE(status["survey_in"]["known"].get<bool>());
  CHECK(status["survey_in"]["valid"].is_null());
  CHECK(status["rtcm"]["valid_frames"] == 0);
  CHECK(status["rtcm"]["last_rx_age_ms"].is_null());
}

TEST_CASE(
    "survey start waits for stop epoch both ACKs and exact USB RAM readback "
    "before "
    "verified") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  VerifyReceiver(pty, base, now);
  ToReadback(pty, base, now);
  CHECK_FALSE(base.StartSurveyIn().has_value());
  CHECK(base.Status(now + 4ms)["configuration"]["verified_age_ms"].is_null());
  auto readback = kSurveyConfig;
  readback[0] = 1;
  readback[1] = 0;
  pty.Send(Ubx(0x06, 0x8b, readback));
  base.Tick(now + 5ms);
  const auto status = base.Status(now + 15ms);
  CHECK(status["configuration"]["state"] == "verified");
  CHECK(status["configuration"]["storage"] == "RAM");
  CHECK(status["configuration"]["survey_min_duration_s"] == 120);
  CHECK(status["configuration"]["survey_accuracy_m"] == 1.25);
  CHECK(status["configuration"]["verified_age_ms"] == 10);
  CHECK_FALSE(status["survey_in"]["completed"].get<bool>());
  CHECK(status["survey_in"]["active"].is_null());
  pty.Send(Survey(false, true));
  pty.Send(kRtcm1005);
  base.Tick(now + 6ms);
  CHECK_FALSE(base.Status(now + 6ms)["survey_in"]["completed"].get<bool>());
  pty.Send(Survey(true, false, 124000));
  base.Tick(now + 7ms);
  CHECK(base.Status(now + 7ms)["survey_in"]["completed"].get<bool>());
}

TEST_CASE("NAK and ACK timeout leave configuration failed") {
  SUBCASE("NAK on disable") {
    Pty pty;
    rtk::BaseStation base({.device = pty.path});
    const auto now = Clock::now();
    VerifyReceiver(pty, base, now);
    REQUIRE(base.StartSurveyIn().has_value());
    base.Tick(now + 2ms);
    pty.Drain();
    pty.Send(Ubx(0x05, 0, {0x06, 0x8a}));
    base.Tick(now + 3ms);
    CHECK(base.Status(now + 3ms)["configuration"]["state"] == "failed");
    CHECK(pty.Drain().empty());
  }
  SUBCASE("unrelated ACK cannot satisfy the request") {
    Pty pty;
    rtk::BaseStation base({.device = pty.path});
    const auto now = Clock::now();
    VerifyReceiver(pty, base, now);
    REQUIRE(base.StartSurveyIn().has_value());
    base.Tick(now + 2ms);
    pty.Drain();
    pty.Send(Ubx(0x05, 1, {0x06, 0x01}));
    base.Tick(now + 3ms);
    CHECK(base.Status(now + 3ms)["configuration"]["state"] == "awaiting_ack");
    base.Tick(now + 1003ms);
    CHECK(base.Status(now + 1003ms)["configuration"]["state"] == "failed");
  }
}

TEST_CASE(
    "missing mismatched and wrong-layer RAM readback cannot verify "
    "configuration") {
  for (unsigned fault = 0; fault < 4; ++fault) {
    CAPTURE(fault);
    Pty pty;
    rtk::BaseStation base({.device = pty.path});
    const auto now = Clock::now();
    VerifyReceiver(pty, base, now);
    ToReadback(pty, base, now);
    auto response = kSurveyConfig;
    response[0] = 1;
    response[1] = 0;
    if (fault == 0) response.back() = 1;
    if (fault == 1) response.resize(response.size() - 8);
    if (fault == 2) response[1] = 1;
    if (fault != 3) pty.Send(Ubx(0x06, 0x8b, response));
    base.Tick(now + (fault == 3 ? 1005ms : 5ms));
    const auto status = base.Status(now + 1005ms);
    CHECK(status["configuration"]["state"] == "failed");
    CHECK(status["configuration"]["verified_age_ms"].is_null());
    CHECK_FALSE(status["last_error"].is_null());
  }
}

TEST_CASE("starting a new survey cannot complete from an old queued NAV-SVIN") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  VerifyReceiver(pty, base, now);
  REQUIRE(base.StartSurveyIn(120, 1.25).has_value());
  pty.Send(Survey(true, false));
  base.Tick(now + 2ms);
  pty.Drain();
  CHECK_FALSE(base.Status(now + 2ms)["survey_in"]["completed"].get<bool>());
  pty.Send(Ubx(0x05, 1, {0x06, 0x8a}));
  pty.Send(Survey(false, false, 124000, 0));
  base.Tick(now + 3ms);
  pty.Drain();
  pty.Send(Ubx(0x05, 1, {0x06, 0x8a}));
  base.Tick(now + 4ms);
  pty.Drain();
  auto readback = kSurveyConfig;
  readback[0] = 1;
  readback[1] = 0;
  pty.Send(Ubx(0x06, 0x8b, readback));
  base.Tick(now + 5ms);
  CHECK(base.Status(now + 5ms)["configuration"]["state"] == "verified");
  CHECK_FALSE(base.Status(now + 5ms)["survey_in"]["completed"].get<bool>());
  CHECK_FALSE(base.Status(now + 5ms)["survey_in"]["known"].get<bool>());
}

TEST_CASE(
    "officially checked 27.12 and captured 9Ps HPG 1.50 support the USB "
    "handshake") {
  for (const auto &version : {MonVer("27.12"), RecordedMonVer()}) {
    Pty pty;
    rtk::BaseStation base({.device = pty.path});
    const auto now = Clock::now();
    base.Tick(now);
    pty.Drain();
    pty.Send(version);
    base.Tick(now + 1ms);
    auto status = base.Status(now + 1ms);
    REQUIRE(status["receiver_verified"].get<bool>());
    if (status["protocol"]["version"] == "27.50") {
      CHECK(status["protocol"]["software"] == "EXT CORE 1.00 (f17067)");
      CHECK(status["protocol"]["hardware"] == "00190000");
      CHECK(status["protocol"]["firmware"] == "HPG 1.50");
    }
    ToReadback(pty, base, now);
    auto response = kSurveyConfig;
    response[0] = 1;
    response[1] = 0;
    pty.Send(Ubx(0x06, 0x8b, response));
    base.Tick(now + 5ms);
    CHECK(base.Status(now + 5ms)["configuration"]["state"] == "verified");
  }
}

TEST_CASE("oversized UBX header does not block later complete valid messages") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  base.Tick(now);
  pty.Drain();
  pty.Send(std::array<std::uint8_t, 6>{0xb5, 0x62, 0x0a, 0x04, 0xff, 0xff});
  pty.Send(MonVer());
  pty.Send(Survey(true, false));
  base.Tick(now + 1ms);
  const auto status = base.Status(now + 1ms);
  CHECK(status["receiver_verified"].get<bool>());
  CHECK(status["survey_in"]["known"].get<bool>());
}

TEST_CASE(
    "an abandoned short UBX frame expires before a later complete reply") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  base.Tick(now);
  pty.Drain();
  const auto long_ver = RecordedMonVer();
  pty.Send(std::span(long_ver).first(7));
  base.Tick(now + 1ms);
  pty.Send(MonVer());
  pty.Send(Survey(true, false));
  base.Tick(now + 1100ms);
  const auto status = base.Status(now + 1100ms);
  CHECK(status["receiver_verified"].get<bool>());
  CHECK(status["survey_in"]["known"].get<bool>());
}

TEST_CASE(
    "reopening a replacement USB endpoint requires a fresh receiver response") {
  char pattern[] = "/tmp/rtk-base-reconnect-XXXXXX";
  const char *directory = ::mkdtemp(pattern);
  REQUIRE(directory != nullptr);
  const auto endpoint = std::filesystem::path(directory) / "receiver";
  Pty first;
  std::filesystem::create_symlink(first.path, endpoint);
  rtk::BaseStation base({.device = endpoint.string()});
  const auto now = Clock::now();
  VerifyReceiver(first, base, now);
  ToReadback(first, base, now);
  auto readback = kSurveyConfig;
  readback[0] = 1;
  readback[1] = 0;
  first.Send(Ubx(0x06, 0x8b, readback));
  base.Tick(now + 5ms);
  first.Send(Survey(true, false));
  first.Send(kRtcm1005);
  base.Tick(now + 6ms);
  REQUIRE(base.Status(now + 6ms)["survey_in"]["completed"].get<bool>());
  ::close(first.master);
  first.master = -1;
  base.Tick(now + 7ms);
  Pty replacement;
  std::filesystem::remove(endpoint);
  std::filesystem::create_symlink(replacement.path, endpoint);
  base.Tick(now + 1008ms);
  auto status = base.Status(now + 1008ms);
  CHECK(status["connected"].get<bool>());
  CHECK_FALSE(status["receiver_verified"].get<bool>());
  CHECK_FALSE(status["survey_in"]["known"].get<bool>());
  CHECK(status["configuration"]["state"] == "idle");
  CHECK(status["configuration"]["verified_age_ms"].is_null());
  CHECK(status["rtcm"]["valid_frames"] == 0);
  CHECK_FALSE(base.StartSurveyIn().has_value());
  replacement.Send(RecordedMonVer());
  base.Tick(now + 1009ms);
  status = base.Status(now + 1009ms);
  CHECK(status["receiver_verified"].get<bool>());
  CHECK_FALSE(status["survey_in"]["completed"].get<bool>());
  replacement.Send(kRtcm1005);
  base.Tick(now + 1010ms);
  const auto frames = base.DrainRtcmFrames();
  REQUIRE(frames.size() == 1);
  CHECK(frames.front().sequence == 2);
  std::filesystem::remove(endpoint);
  std::filesystem::remove(directory);
}

TEST_CASE("cancelled configuration cannot advance from a late disable ACK") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  VerifyReceiver(pty, base, now);
  REQUIRE(base.StartSurveyIn(120, 1.25).has_value());
  base.Tick(now + 2ms);
  REQUIRE(pty.Drain() == Ubx(0x06, 0x8a, {0, 1, 0, 0, 1, 0, 3, 0x20, 0}));
  base.CancelPendingConfiguration();
  CHECK(base.Status(now + 2ms)["configuration"]["state"] == "failed");
  pty.Send(Ubx(0x05, 1, {0x06, 0x8a}));
  base.Tick(now + 300ms);
  CHECK(pty.Drain().empty());
  CHECK(base.Status(now + 300ms)["configuration"]["state"] == "failed");
}

TEST_CASE(
    "cancelling readback does not send writes to undo an already applied "
    "survey") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  VerifyReceiver(pty, base, now);
  ToReadback(pty, base, now);
  base.CancelPendingConfiguration();
  auto readback = kSurveyConfig;
  readback[0] = 1;
  readback[1] = 0;
  pty.Send(Ubx(0x06, 0x8b, readback));
  base.Tick(now + 300ms);
  CHECK(pty.Drain().empty());
  CHECK(base.Status(now + 300ms)["configuration"]["state"] == "failed");
  CHECK(base.Status(now + 300ms)["configuration"]["verified_age_ms"].is_null());
}

TEST_CASE("cancellation leaves a verified configuration intact") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  VerifyReceiver(pty, base, now);
  ToReadback(pty, base, now);
  auto readback = kSurveyConfig;
  readback[0] = 1;
  readback[1] = 0;
  pty.Send(Ubx(0x06, 0x8b, readback));
  base.Tick(now + 5ms);
  base.CancelPendingConfiguration();
  CHECK(base.Status(now + 5ms)["configuration"]["state"] == "verified");
  CHECK(pty.Drain().empty());
}

TEST_CASE(
    "disable ACK waits for a later inactive NAV-SVIN epoch before enable") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  VerifyReceiver(pty, base, now);
  pty.Send(Survey(false, true, 100000, 9622));
  base.Tick(now + 2ms);
  REQUIRE(base.StartSurveyIn(120, 1.25).has_value());
  base.Tick(now + 3ms);
  REQUIRE(pty.Drain() == Ubx(0x06, 0x8a, {0, 1, 0, 0, 1, 0, 3, 0x20, 0}));
  pty.Send(Ubx(0x05, 1, {0x06, 0x8a}));
  pty.Send(Survey(false, true, 101000, 9623));
  base.Tick(now + 4ms);
  CHECK(base.Status(now + 4ms)["configuration"]["state"] == "awaiting_stop");
  CHECK(pty.Drain().empty());
  CHECK_FALSE(base.StartSurveyIn().has_value());

  pty.Send(Survey(false, false, 100000, 0));
  pty.Send(Survey(false, false, 101000, 0));
  base.Tick(now + 5ms);
  CHECK(pty.Drain().empty());
  CHECK(base.Status(now + 5ms)["configuration"]["state"] == "awaiting_stop");

  pty.Send(Survey(false, false, 102000, 0));
  base.Tick(now + 700ms);
  CHECK(pty.Drain() == Ubx(0x06, 0x8a, kSurveyConfig));
  CHECK(base.Status(now + 700ms)["configuration"]["state"] == "awaiting_ack");
  pty.Send(Ubx(0x05, 1, {0x06, 0x8a}));
  base.Tick(now + 701ms);
  CHECK(pty.Drain() == Ubx(0x06, 0x8b, kReadKeys));
  auto readback = kSurveyConfig;
  readback[0] = 1;
  readback[1] = 0;
  pty.Send(Ubx(0x06, 0x8b, readback));
  base.Tick(now + 702ms);
  CHECK(base.Status(now + 702ms)["configuration"]["state"] == "verified");
  CHECK_FALSE(base.Status(now + 702ms)["survey_in"]["known"].get<bool>());
  pty.Send(Survey(false, true, 103000, 1));
  base.Tick(now + 703ms);
  CHECK(base.Status(now + 703ms)["survey_in"]["duration_s"] == 1);
  CHECK(base.Status(now + 703ms)["survey_in"]["active"] == true);
}

TEST_CASE(
    "minimum freshness window permits a new navigation epoch during "
    "configuration") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path, .stale_after_ms = 1000});
  const auto now = Clock::now();
  VerifyReceiver(pty, base, now);
  pty.Send(Survey(false, true, 100000));
  base.Tick(now + 2ms);
  REQUIRE(base.StartSurveyIn(120, 1.25).has_value());
  base.Tick(now + 3ms);
  pty.Drain();
  pty.Send(Ubx(0x05, 1, {0x06, 0x8a}));
  base.Tick(now + 4ms);
  REQUIRE(base.Status(now + 4ms)["configuration"]["state"] == "awaiting_stop");
  base.Tick(now + 600ms);
  auto polls = Ubx(0x0a, 0x04, {});
  const auto survey_poll = Ubx(0x01, 0x3b, {});
  polls.insert(polls.end(), survey_poll.begin(), survey_poll.end());
  REQUIRE(pty.Drain() == polls);
  pty.Send(MonVer());
  base.Tick(now + 700ms);
  pty.Send(Survey(false, false, 101000, 0));
  base.Tick(now + 1200ms);
  CHECK(base.Status(now + 1200ms)["receiver_verified"] == true);
  CHECK(base.Status(now + 1200ms)["configuration"]["state"] == "awaiting_ack");
}

TEST_CASE("inactive data queued before disable ACK does not confirm stop") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  VerifyReceiver(pty, base, now);
  pty.Send(Survey(false, true, 100000));
  base.Tick(now + 2ms);
  REQUIRE(base.StartSurveyIn(120, 1.25).has_value());
  base.Tick(now + 3ms);
  pty.Drain();
  pty.Send(Survey(false, false, 101000, 0));
  pty.Send(Ubx(0x05, 1, {0x06, 0x8a}));
  pty.Send(Survey(false, false, 101000, 0));
  base.Tick(now + 4ms);
  CHECK(base.Status(now + 4ms)["configuration"]["state"] == "awaiting_stop");
  CHECK(pty.Drain().empty());
  pty.Send(Survey(false, false, 102000, 0));
  base.Tick(now + 5ms);
  CHECK(pty.Drain() == Ubx(0x06, 0x8a, kSurveyConfig));
}

TEST_CASE("stop confirmation timeout and cancellation never enable survey") {
  SUBCASE("timeout rejects inactive data arriving after the stop deadline") {
    Pty pty;
    rtk::BaseStation base({.device = pty.path, .stale_after_ms = 10000});
    const auto now = Clock::now();
    VerifyReceiver(pty, base, now);
    pty.Send(Survey(false, true));
    base.Tick(now + 2ms);
    REQUIRE(base.StartSurveyIn().has_value());
    base.Tick(now + 3ms);
    pty.Drain();
    pty.Send(Ubx(0x05, 1, {0x06, 0x8a}));
    base.Tick(now + 4ms);
    pty.Drain();
    pty.Send(Survey(false, false, 124000, 0));
    base.Tick(now + 3004ms);
    const auto status = base.Status(now + 3004ms);
    CHECK(status["configuration"]["state"] == "failed");
    CHECK(status["last_error"] == "survey stop confirmation timed out");
    auto polls = Ubx(0x0a, 0x04, {});
    const auto survey_poll = Ubx(0x01, 0x3b, {});
    polls.insert(polls.end(), survey_poll.begin(), survey_poll.end());
    CHECK(pty.Drain() == polls);
  }
  SUBCASE("cancelled stop ignores later inactive data and ACK") {
    Pty pty;
    rtk::BaseStation base({.device = pty.path});
    const auto now = Clock::now();
    VerifyReceiver(pty, base, now);
    REQUIRE(base.StartSurveyIn().has_value());
    base.Tick(now + 2ms);
    pty.Drain();
    pty.Send(Ubx(0x05, 1, {0x06, 0x8a}));
    base.Tick(now + 3ms);
    pty.Drain();
    REQUIRE(base.Status(now + 3ms)["configuration"]["state"] ==
            "awaiting_stop");
    base.CancelPendingConfiguration();
    pty.Send(Survey(false, false, 124000, 0));
    pty.Send(Survey(false, false, 125000, 0));
    pty.Send(Ubx(0x05, 1, {0x06, 0x8a}));
    base.Tick(now + 300ms);
    CHECK(pty.Drain().empty());
    CHECK(base.Status(now + 300ms)["configuration"]["state"] == "failed");
    CHECK(base.Status(now + 300ms)["last_error"] == "configuration cancelled");
  }
}

TEST_CASE("stop without prior NAV-SVIN first establishes an epoch baseline") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  VerifyReceiver(pty, base, now);
  REQUIRE(base.StartSurveyIn(120, 1.25).has_value());
  base.Tick(now + 2ms);
  pty.Drain();
  pty.Send(Ubx(0x05, 1, {0x06, 0x8a}));
  pty.Send(Survey(false, false, 123000, 0));
  base.Tick(now + 3ms);
  CHECK(base.Status(now + 3ms)["configuration"]["state"] == "awaiting_stop");
  CHECK(pty.Drain().empty());
  pty.Send(Survey(false, false, 124000, 0));
  base.Tick(now + 4ms);
  CHECK(pty.Drain() == Ubx(0x06, 0x8a, kSurveyConfig));
}

TEST_CASE(
    "stop confirmation accepts a newer epoch across the GPS week boundary") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  VerifyReceiver(pty, base, now);
  pty.Send(Survey(false, true, 604799000));
  base.Tick(now + 2ms);
  REQUIRE(base.StartSurveyIn(120, 1.25).has_value());
  base.Tick(now + 3ms);
  pty.Drain();
  pty.Send(Ubx(0x05, 1, {0x06, 0x8a}));
  base.Tick(now + 4ms);
  CHECK(pty.Drain().empty());
  pty.Send(Survey(false, false, 0, 0));
  base.Tick(now + 5ms);
  CHECK(pty.Drain() == Ubx(0x06, 0x8a, kSurveyConfig));
}
}  // namespace

TEST_CASE("RTCM drain excludes corrupt frames and bounds pending data") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  base.Tick(now);
  pty.Drain();
  auto bad = kRtcm1005;
  bad.back() ^= 1;
  pty.Send(bad);
  base.Tick(now + 1ms);
  CHECK(base.DrainRtcmFrames().empty());
  for (unsigned i = 0; i < 35; ++i) pty.Send(kRtcm1005);
  base.Tick(now + 2ms);
  auto frames = base.DrainRtcmFrames();
  REQUIRE(frames.size() == 32);
  CHECK(frames.front().sequence == 4);
  CHECK(frames.back().sequence == 35);
  for (const auto &frame : frames) {
    CHECK(frame.message_type == 1005);
    CHECK(frame.bytes ==
          std::vector<std::uint8_t>(kRtcm1005.begin(), kRtcm1005.end()));
    CHECK(frame.received_at == now + 2ms);
  }
  CHECK(base.Status(now + 2ms)["rtcm"]["queue_dropped"] == 3);
  CHECK(base.DrainRtcmFrames().empty());
  pty.Send(kRtcm1005);
  base.Tick(now + 3ms);
  ::close(pty.master);
  pty.master = -1;
  base.Tick(now + 4ms);
  CHECK(base.DrainRtcmFrames().empty());
}
TEST_CASE("NAV-SVIN raw sample is the actual complete wire frame") {
  Pty pty;
  rtk::BaseStation base({.device = pty.path});
  const auto now = Clock::now();
  base.Tick(now);
  pty.Drain();
  const auto wire = Survey(true, false);
  pty.Send(wire);
  base.Tick(now + 1ms);
  const auto raw =
      base.Status(now + 11ms)["survey_in"]["raw_hex"].get<std::string>();
  REQUIRE(raw.size() == wire.size() * 2);
  for (std::size_t i = 0; i < wire.size(); ++i)
    CHECK(std::stoul(raw.substr(i * 2, 2), nullptr, 16) == wire[i]);
  CHECK(base.Status(now + 11ms)["survey_in"]["last_sample_age_ms"] == 10);
}
