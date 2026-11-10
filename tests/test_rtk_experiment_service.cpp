/** @file test_rtk_experiment_service.cpp @brief 真实PTY验证箱内RTK附属服务，不建立MQTT。 */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <span>
#include <vector>

#include "common/mavlink.h"
#include "config/app_config.hpp"
#include "logging/logger.hpp"
#include "rtk/experiment_service.hpp"
#include "uart/mavlink_link.hpp"

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
  ~Pty() { if (master >= 0) ::close(master); }
  Pty(const Pty&) = delete;
  Pty& operator=(const Pty&) = delete;
  void Send(std::span<const std::uint8_t> bytes) const {
    REQUIRE(::write(master, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  }
  void Send(const mavlink_message_t& message) const {
    std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> bytes{};
    const auto length = mavlink_msg_to_send_buffer(bytes.data(), &message);
    Send(std::span<const std::uint8_t>(bytes).first(length));
  }
  std::vector<std::uint8_t> Drain() const {
    std::vector<std::uint8_t> result;
    std::array<std::uint8_t, 4096> bytes{};
    for (ssize_t count; (count = ::read(master, bytes.data(), bytes.size())) > 0;)
      result.insert(result.end(), bytes.begin(), bytes.begin() + count);
    return result;
  }
};

std::vector<std::uint8_t> ReceiverVersion() {
  std::ifstream file(std::string(SOURCE_DIR) + "/tests/fixtures/rtk/9ps_mon_ver_hpg_1_50.hex");
  REQUIRE(file.good());
  std::vector<std::uint8_t> bytes;
  std::string hex;
  file >> hex;
  for (std::size_t offset = 0; offset < hex.size(); offset += 2) {
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(offset, 2), nullptr, 16)));
  }
  return bytes;
}

struct Fixture {
  Pty base, px4;
  std::ostringstream output, error;
  std::unique_ptr<logging::Logger> logger;
  config::RtkBaseConfig settings;
  Fixture() {
    auto created = logging::Logger::Create({logging::Level::kInfo, {}, 0}, output, error);
    REQUIRE(created);
    logger = std::move(*created);
    settings.enabled = true;
    settings.device = base.path;
    settings.px4_device = px4.path;
    settings.px4_baud = 115200;
  }
};

mavlink_message_t Heartbeat(std::uint8_t system_id = 1) {
  mavlink_message_t message{};
  mavlink_msg_heartbeat_pack(system_id, MAV_COMP_ID_AUTOPILOT1, &message,
      MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_PX4, 0, 0, MAV_STATE_STANDBY);
  return message;
}

mavlink_message_t Identity() {
  mavlink_message_t message{};
  mavlink_open_drone_id_basic_id_t identity{};
  identity.id_type = MAV_ODID_ID_TYPE_SERIAL_NUMBER;
  std::memcpy(identity.uas_id, "PX4-OBSERVATION", 15);
  mavlink_msg_open_drone_id_basic_id_encode(1, MAV_COMP_ID_AUTOPILOT1, &message, &identity);
  return message;
}

mavlink_message_t Gps(std::uint8_t system_id = 1) {
  mavlink_message_t message{};
  mavlink_gps_raw_int_t gps{};
  gps.lat = 234567890;
  gps.lon = 1131234567;
  gps.fix_type = 6;
  mavlink_msg_gps_raw_int_encode(system_id, MAV_COMP_ID_AUTOPILOT1, &message, &gps);
  return message;
}

std::vector<std::uint8_t> Survey(bool valid, std::uint32_t tow = 123000) {
  std::vector<std::uint8_t> frame(48);
  frame[0] = 0xb5; frame[1] = 0x62; frame[2] = 1; frame[3] = 0x3b; frame[4] = 40;
  for (unsigned byte = 0; byte < 4; ++byte) frame[10 + byte] = tow >> (8 * byte);
  frame[14] = 61; frame[38] = 60; frame[42] = valid;
  std::uint8_t a = 0, b = 0;
  for (std::size_t i = 2; i < 46; ++i) { a += frame[i]; b += a; }
  frame[46] = a; frame[47] = b;
  return frame;
}

// 固定CRC24Q封装样本，测试不将两字节负载冒充完整1005业务数据。
const std::vector<std::uint8_t> kRtcm{0xd3, 0, 2, 0x3e, 0xd0, 0xa4, 0xe0, 0};

std::vector<uart::WireFrame> ReadFrames(const Pty& pty) {
  uart::MavlinkFrameAssembler assembler;
  std::vector<uart::WireFrame> frames;
  auto frame = assembler.FeedFrame(pty.Drain(), Clock::now());
  while (frame) {
    frames.push_back(std::move(*frame));
    frame = assembler.FeedFrame({}, Clock::now());
  }
  return frames;
}
}  // namespace

TEST_CASE("PX4缺失时基站仍可真实识别和接受RAM测量配置") {
  Fixture fixture;
  fixture.settings.px4_device = "/tmp/cns-rtk-test-no-px4-endpoint";
  rtk::ExperimentService service(fixture.settings, *fixture.logger);
  const auto now = Clock::now();
  service.Tick(now);
  fixture.base.Drain();
  fixture.base.Send(ReceiverVersion());
  service.Tick(now + 10ms);
  const auto status = service.Status(now + 10ms);
  CHECK_FALSE(status.contains("device_id"));
  CHECK_FALSE(status.contains("device_type"));
  CHECK_FALSE(status["px4"]["serial_connected"].get<bool>());
  CHECK(status["px4"]["device_id"].is_null());
  REQUIRE(status["base"].contains("receiver_verified"));
  CHECK(status["base"]["receiver_verified"] == true);
  CHECK(service.BaseStatus(now + 10ms) == status["base"]);
  CHECK(service.StartSurveyIn(60, 2.0));
  service.Tick(now + 20ms);
  CHECK(service.BaseStatus(now + 20ms)["configuration"]["state"] == "awaiting_ack");
}

TEST_CASE("树莓派启动默认不转发且提供独立移动站观测") {
  Fixture fixture;
  rtk::ExperimentService service(fixture.settings, *fixture.logger);
  const auto now = Clock::now();
  service.Tick(now);
  const auto status = service.Status(now);
  REQUIRE(status.contains("relay"));
  CHECK(status["relay"]["state"] == "stopped");
  CHECK(status["relay"]["forwarded_frames"] == 0);
  CHECK(status["px4"]["gps_observation"]["known"] == false);
}

TEST_CASE("CRC合法改正经真实PTY转发且暂停恢复不重放旧帧") {
  Fixture fixture;
  rtk::ExperimentService service(fixture.settings, *fixture.logger);
  const auto now = Clock::now();
  service.Tick(now);
  fixture.base.Send(ReceiverVersion());
  fixture.base.Send(Survey(true));
  fixture.px4.Send(Heartbeat());
  fixture.px4.Send(Identity());
  service.Tick(now + 10ms);
  fixture.px4.Drain();
  REQUIRE(service.ControlRelay("rtk_rover_start"));
  fixture.base.Send(kRtcm);
  service.Tick(now + 20ms);
  auto frames = ReadFrames(fixture.px4);
  REQUIRE(frames.size() == 1);
  REQUIRE(static_cast<unsigned>(frames[0].message.msgid) == MAVLINK_MSG_ID_GPS_RTCM_DATA);
  mavlink_gps_rtcm_data_t data{};
  mavlink_msg_gps_rtcm_data_decode(&frames[0].message, &data);
  CHECK(data.flags == 0);
  CHECK(data.len == kRtcm.size());
  CHECK(std::vector(data.data, data.data + data.len) == kRtcm);
  auto status = service.Status(now + 20ms);
  CHECK(status["relay"]["forwarded_frames"] == 1);
  CHECK(status["observation"]["latest_rtcm"]["raw_hex"] == "d300023ed0a4e000");
  CHECK(status["observation"]["latest_transfer"]["complete"] == true);
  CHECK(status["observation"]["latest_transfer"]["run_sequence"] == 1);
  CHECK(status["observation"]["latest_transfer"]["fragments"][0]["wire_length_bytes"] == frames[0].bytes.size());
  CHECK(status["relay"]["receiver_ack_available"] == false);

  REQUIRE(service.ControlRelay("rtk_rover_pause"));
  fixture.base.Send(kRtcm);
  fixture.px4.Send(Gps());
  service.Tick(now + 30ms);
  CHECK(ReadFrames(fixture.px4).empty());
  CHECK(service.Status(now + 30ms)["px4"]["gps_observation"]["known"] == true);
  CHECK(service.Status(now + 30ms)["relay"]["state"] == "paused");
  REQUIRE(service.ControlRelay("rtk_rover_resume"));
  service.Tick(now + 40ms);
  CHECK(ReadFrames(fixture.px4).empty());
  CHECK(service.Status(now + 40ms)["relay"]["forwarded_frames"] == 1);
  auto bad = kRtcm;
  bad.at(7) ^= 1;
  fixture.base.Send(bad);
  fixture.base.Send(kRtcm);
  service.Tick(now + 50ms);
  frames = ReadFrames(fixture.px4);
  REQUIRE(frames.size() == 1);
  CHECK(static_cast<unsigned>(frames[0].message.msgid) == MAVLINK_MSG_ID_GPS_RTCM_DATA);
  status = service.Status(now + 50ms);
  CHECK(status["relay"]["forwarded_frames"] == 2);
  CHECK(status["base"]["rtcm"]["crc_errors"] == 1);
  CHECK(status["relay"]["dropped_inactive"] == 1);
  REQUIRE(service.ControlRelay("rtk_rover_stop"));
  fixture.base.Send(kRtcm);
  service.Tick(now + 60ms);
  CHECK(ReadFrames(fixture.px4).empty());
  const auto compact = service.Status(now + 60ms, false);
  CHECK(compact["observation"]["samples_included"] == false);
  CHECK_FALSE(compact["observation"].contains("latest_transfer"));
  CHECK_FALSE(compact["base"]["survey_in"].contains("raw_hex"));
  CHECK_FALSE(compact["px4"]["gps_observation"].contains("raw_hex"));
}

TEST_CASE("基站未完成及GPS失效不能作为有效转发或当前Fixed") {
  Fixture fixture;
  rtk::ExperimentService service(fixture.settings, *fixture.logger);
  const auto now = Clock::now();
  service.Tick(now);
  fixture.base.Send(ReceiverVersion());
  fixture.base.Send(Survey(false));
  fixture.px4.Send(Heartbeat()); fixture.px4.Send(Identity()); fixture.px4.Send(Gps());
  service.Tick(now + 10ms);
  fixture.px4.Drain();
  REQUIRE(service.ControlRelay("rtk_rover_start"));
  fixture.base.Send(kRtcm);
  service.Tick(now + 20ms);
  CHECK(ReadFrames(fixture.px4).empty());
  CHECK(service.Status(now + 20ms)["relay"]["state"] == "waiting_base");
  fixture.px4.Send(Heartbeat());
  service.Tick(now + 4s);
  const auto status = service.Status(now + 4s);
  CHECK(status["px4"]["identity_verified"] == true);
  CHECK(status["px4"]["gps_observation"]["stale"] == true);
  CHECK(status["px4"]["gps_observation"]["fix_type"].is_null());
  CHECK_FALSE(status["px4"]["telemetry"].contains("gps"));
  CHECK(status["relay"]["forwarded_frames"] == 0);
}

TEST_CASE("待发队列过期丢弃且重做基站测量会暂停改正") {
  Fixture fixture;
  rtk::ExperimentService service(fixture.settings, *fixture.logger);
  const auto now = Clock::now();
  service.Tick(now);
  fixture.base.Send(ReceiverVersion()); fixture.base.Send(Survey(true));
  fixture.px4.Send(Heartbeat()); fixture.px4.Send(Identity());
  service.Tick(now + 10ms);
  fixture.px4.Drain();
  REQUIRE(service.ControlRelay("rtk_rover_start"));
  fixture.base.Send(kRtcm); fixture.base.Send(kRtcm); fixture.base.Send(kRtcm);
  service.Tick(now + 20ms);
  REQUIRE(service.Status(now + 20ms)["relay"]["pending_frames"] == 2);
  ReadFrames(fixture.px4);
  service.Tick(now + 1200ms);
  CHECK(service.Status(now + 1200ms)["relay"]["dropped_stale"] == 2);
  CHECK(service.Status(now + 1200ms)["relay"]["forwarded_frames"] == 1);
  REQUIRE(service.StartSurveyIn(60, 2.0));
  CHECK(service.Status(now + 1200ms)["relay"]["state"] == "paused");
}

TEST_CASE("PX4身份遥测仅作为下挂观测且独占自己的MAVLink端点") {
  Fixture fixture;
  rtk::ExperimentService service(fixture.settings, *fixture.logger);
  const auto now = Clock::now();
  service.Tick(now);
  fixture.px4.Drain();
  fixture.px4.Send(Gps());
  fixture.px4.Send(Heartbeat());
  service.Tick(now + 10ms);
  auto status = service.Status(now + 10ms);
  CHECK(status["px4"]["serial_connected"] == true);
  CHECK(status["px4"]["identity_verified"] == false);
  CHECK(status["px4"]["telemetry"].empty());
  fixture.px4.Send(Identity());
  fixture.px4.Send(Gps(2));
  service.Tick(now + 20ms);
  status = service.Status(now + 20ms);
  REQUIRE(status["px4"]["identity_verified"] == true);
  CHECK(status["px4"]["device_id"] == "PX4-OBSERVATION");
  CHECK(status["px4"]["system_id"] == 1);
  CHECK(status["px4"]["component_id"] == MAV_COMP_ID_AUTOPILOT1);
  CHECK_FALSE(status["px4"]["telemetry"].contains("gps"));
  fixture.px4.Send(Gps());
  service.Tick(now + 30ms);
  status = service.Status(now + 30ms);
  CHECK(status["px4"]["telemetry"]["gps"]["lat"] == doctest::Approx(23.456789));
  CHECK_FALSE(status.contains("device_id"));
  CHECK_FALSE(status.contains("device_type"));
}

TEST_CASE("PX4心跳超时只清自身实时观测并继续处理独立基站") {
  Fixture fixture;
  rtk::ExperimentService service(fixture.settings, *fixture.logger);
  const auto now = Clock::now();
  service.Tick(now);
  fixture.px4.Drain();
  fixture.px4.Send(Heartbeat());
  fixture.px4.Send(Identity());
  fixture.px4.Send(Gps());
  service.Tick(now + 10ms);
  REQUIRE(service.Status(now + 10ms)["px4"]["identity_verified"] == true);
  fixture.base.Send(ReceiverVersion());
  service.Tick(now + 6s);
  const auto status = service.Status(now + 6s);
  CHECK(status["px4"]["identity_verified"] == false);
  CHECK(status["px4"]["device_id"].is_null());
  CHECK(status["px4"]["telemetry"].empty());
  CHECK(status["base"]["connected"] == true);
  CHECK(status["base"]["receiver_verified"] == true);
  CHECK(service.StartSurveyIn(90, 1.5));
}

TEST_CASE("实验取消转交基站且迟到配置ACK不能推进后续写入") {
  Fixture fixture;
  rtk::ExperimentService service(fixture.settings, *fixture.logger);
  const auto now = Clock::now();
  service.Tick(now);
  fixture.base.Send(ReceiverVersion());
  service.Tick(now + 10ms);
  REQUIRE(service.StartSurveyIn(60, 2.0));
  service.Tick(now + 20ms);
  fixture.base.Drain();
  service.CancelPendingConfiguration();
  REQUIRE(service.BaseStatus(now + 20ms).contains("configuration"));
  CHECK(service.BaseStatus(now + 20ms)["configuration"]["state"] == "failed");
  // UBX-ACK-ACK 对 CFG-VALSET 的完整合法线上字节。
  fixture.base.Send(std::array<std::uint8_t, 10>{0xb5, 0x62, 0x05, 0x01, 0x02, 0x00, 0x06, 0x8a, 0x98, 0xc1});
  service.Tick(now + 30ms);
  CHECK(fixture.base.Drain().empty());
  CHECK(service.BaseStatus(now + 30ms)["configuration"]["state"] == "failed");
}

TEST_CASE("activation drains unread corrections and rejects a frame spanning resume") {
  Fixture fixture;
  rtk::ExperimentService service(fixture.settings, *fixture.logger);
  const auto now = Clock::now();
  service.Tick(now);
  fixture.base.Send(ReceiverVersion()); fixture.base.Send(Survey(true));
  fixture.px4.Send(Heartbeat()); fixture.px4.Send(Identity());
  service.Tick(now + 10ms); fixture.px4.Drain();
  fixture.base.Send(kRtcm);
  REQUIRE(service.ControlRelay("rtk_rover_start"));
  service.Tick(now + 20ms);
  CHECK(ReadFrames(fixture.px4).empty());
  REQUIRE(service.ControlRelay("rtk_rover_pause"));
  fixture.base.Send(kRtcm);
  fixture.base.Send(std::span(kRtcm).first(4));
  REQUIRE(service.ControlRelay("rtk_rover_resume"));
  fixture.base.Send(std::span(kRtcm).subspan(4));
  service.Tick(now + 30ms);
  CHECK(ReadFrames(fixture.px4).empty());
  fixture.base.Send(kRtcm);
  service.Tick(now + 40ms);
  const auto forwarded = ReadFrames(fixture.px4);
  REQUIRE(forwarded.size() == 1);
  CHECK(static_cast<unsigned>(forwarded.front().message.msgid) == MAVLINK_MSG_ID_GPS_RTCM_DATA);
  CHECK(service.Status(now + 40ms)["relay"]["forwarded_frames"] == 1);
}

TEST_CASE("identity recovery cannot authorize corrections read in its own receive pass") {
  Fixture fixture;
  rtk::ExperimentService service(fixture.settings, *fixture.logger);
  const auto now = Clock::now();
  service.Tick(now);
  REQUIRE(service.ControlRelay("rtk_rover_start"));
  fixture.base.Send(ReceiverVersion()); fixture.base.Send(Survey(true));
  fixture.base.Send(kRtcm);
  fixture.px4.Send(Heartbeat()); fixture.px4.Send(Identity());
  service.Tick(now + 10ms);
  auto frames = ReadFrames(fixture.px4);
  CHECK(std::none_of(frames.begin(), frames.end(), [](const auto& frame) {
    return frame.message.msgid == MAVLINK_MSG_ID_GPS_RTCM_DATA;
  }));
  fixture.base.Send(kRtcm);
  service.Tick(now + 20ms);
  REQUIRE(ReadFrames(fixture.px4).size() == 1);
  CHECK(service.Status(now + 20ms)["relay"]["forwarded_frames"] == 1);
  fixture.base.Send(ReceiverVersion()); fixture.base.Send(Survey(true, 124000));
  fixture.base.Send(kRtcm);
  fixture.px4.Send(Heartbeat()); fixture.px4.Send(Identity());
  service.Tick(now + 6s);
  frames = ReadFrames(fixture.px4);
  CHECK(std::none_of(frames.begin(), frames.end(), [](const auto& frame) {
    return frame.message.msgid == MAVLINK_MSG_ID_GPS_RTCM_DATA;
  }));
  fixture.base.Send(kRtcm);
  service.Tick(now + 6010ms);
  REQUIRE(ReadFrames(fixture.px4).size() == 1);
  CHECK(service.Status(now + 6010ms)["relay"]["forwarded_frames"] == 2);
}

TEST_CASE("relay waits after one second even while receiver observations remain fresh") {
  Fixture fixture;
  fixture.settings.stale_after_ms = 6000;
  rtk::ExperimentService service(fixture.settings, *fixture.logger);
  const auto now = Clock::now();
  service.Tick(now);
  fixture.base.Send(ReceiverVersion()); fixture.base.Send(Survey(true));
  fixture.px4.Send(Heartbeat()); fixture.px4.Send(Identity());
  service.Tick(now + 10ms); fixture.px4.Drain();
  REQUIRE(service.ControlRelay("rtk_rover_start"));
  fixture.base.Send(kRtcm); service.Tick(now + 20ms);
  const auto status = service.Status(now + 2020ms);
  CHECK(status["base"]["rtcm"]["stale"] == false);
  CHECK(status["relay"]["state"] == "waiting_rtcm");
}

TEST_CASE("run transitions exclude previous students and stopped elapsed time freezes") {
  Fixture fixture;
  rtk::ExperimentService service(fixture.settings, *fixture.logger);
  const auto now = Clock::now();
  service.Tick(now);
  fixture.px4.Send(Heartbeat()); fixture.px4.Send(Identity());
  service.Tick(now + 10ms);
  REQUIRE(service.ControlRelay("rtk_rover_start", now + 10ms));
  auto gps = Gps();
  fixture.px4.Send(gps); service.Tick(now + 20ms);
  REQUIRE(service.Status(now + 20ms)["px4"]["gps_observation"]["transitions"].size() == 1);
  REQUIRE(service.ControlRelay("rtk_rover_stop", now + 20ms));
  const auto stopped = service.Status(now + 20ms)["relay"]["run_elapsed_ms"];
  CHECK(service.Status(now + 1020ms)["relay"]["run_elapsed_ms"] == stopped);
  service.Tick(now + 1020ms);
  REQUIRE(service.ControlRelay("rtk_rover_start", now + 1020ms));
  auto status = service.Status(now + 1020ms);
  CHECK(status["px4"]["gps_observation"]["known"] == true);
  CHECK(status["px4"]["gps_observation"]["transitions"].empty());
  mavlink_gps_raw_int_t sample{};
  sample.time_usec = 2; sample.fix_type = 5;
  mavlink_msg_gps_raw_int_encode(1, MAV_COMP_ID_AUTOPILOT1, &gps, &sample);
  fixture.px4.Send(gps); service.Tick(now + 1120ms);
  status = service.Status(now + 1120ms);
  REQUIRE(status["px4"]["gps_observation"]["transitions"].size() == 1);
  CHECK(status["px4"]["gps_observation"]["transitions"][0]["run_sequence"] == 2);
  CHECK(status["px4"]["gps_observation"]["transitions"][0]["run_elapsed_ms"] == 100);
}

TEST_CASE("duplicate GPS epoch keeps telemetry observation and raw sample consistent") {
  Fixture fixture;
  rtk::ExperimentService service(fixture.settings, *fixture.logger);
  const auto now = Clock::now();
  service.Tick(now);
  fixture.px4.Send(Heartbeat()); fixture.px4.Send(Identity());
  mavlink_gps_raw_int_t gps{};
  gps.time_usec = 123; gps.fix_type = 5;
  mavlink_message_t message{};
  mavlink_msg_gps_raw_int_encode(1, MAV_COMP_ID_AUTOPILOT1, &message, &gps);
  fixture.px4.Send(message); service.Tick(now + 10ms);
  const auto raw = service.Status(now + 10ms)["px4"]["gps_observation"]["raw_hex"];
  gps.fix_type = 6;
  mavlink_msg_gps_raw_int_encode(1, MAV_COMP_ID_AUTOPILOT1, &message, &gps);
  fixture.px4.Send(message); service.Tick(now + 1010ms);
  const auto status = service.Status(now + 1010ms);
  CHECK(status["px4"]["telemetry"]["gps"]["fix_type"] == 5);
  CHECK(status["px4"]["gps_observation"]["fix_type"] == 5);
  CHECK(status["px4"]["gps_observation"]["raw_hex"] == raw);
  CHECK(status["px4"]["gps_observation"]["sample_age_ms"] == 1000);
  CHECK(status["px4"]["gps_observation"]["frame_age_ms"] == 0);
}
