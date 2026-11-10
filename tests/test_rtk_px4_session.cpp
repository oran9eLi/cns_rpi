#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "rtk/px4_session.hpp"

using namespace std::chrono_literals;

namespace {
const auto kStart = rtk::Px4Session::Clock::time_point{} + 10s;

mavlink_message_t Heartbeat(std::uint8_t sysid = 42,
                            std::uint8_t compid = MAV_COMP_ID_AUTOPILOT1,
                            std::uint8_t autopilot = MAV_AUTOPILOT_PX4,
                            std::uint8_t type = MAV_TYPE_QUADROTOR) {
  mavlink_message_t message{};
  mavlink_msg_heartbeat_pack(sysid, compid, &message, type, autopilot,
                             0, 0, MAV_STATE_STANDBY);
  return message;
}

mavlink_message_t BasicId(const char* identity = "PX4-EXISTING-ID",
                          std::uint8_t sysid = 42,
                          std::uint8_t compid = MAV_COMP_ID_AUTOPILOT1) {
  mavlink_message_t message{};
  mavlink_open_drone_id_basic_id_t data{};
  data.id_type = MAV_ODID_ID_TYPE_SERIAL_NUMBER;
  std::memcpy(data.uas_id, identity, std::min(std::strlen(identity), sizeof(data.uas_id)));
  mavlink_msg_open_drone_id_basic_id_encode(sysid, compid, &message, &data);
  return message;
}

mavlink_message_t Gps(std::uint8_t sysid = 42,
                      std::uint8_t compid = MAV_COMP_ID_AUTOPILOT1,
                      std::int32_t latitude = 234567890) {
  mavlink_message_t message{};
  mavlink_gps_raw_int_t data{};
  data.fix_type = 6;
  data.lat = latitude;
  data.lon = 1131234567;
  data.satellites_visible = 19;
  mavlink_msg_gps_raw_int_encode(sysid, compid, &message, &data);
  return message;
}
}  // namespace

TEST_CASE("RTK会话只接受PX4飞控端点的心跳") {
  rtk::Px4Session session;
  CHECK_FALSE(session.Observe(Heartbeat(42, MAV_COMP_ID_MISSIONPLANNER), kStart));
  CHECK_FALSE(session.Observe(Heartbeat(42, 1, MAV_AUTOPILOT_INVALID,
                                       MAV_TYPE_ONBOARD_CONTROLLER), kStart));
  CHECK_FALSE(session.Observe(Heartbeat(0), kStart));
  CHECK_FALSE(session.Endpoint());
  CHECK(session.Observe(Heartbeat(), kStart));
  REQUIRE(session.Endpoint());
  CHECK(session.Endpoint()->system_id == 42);
  CHECK(session.Endpoint()->component_id == MAV_COMP_ID_AUTOPILOT1);
  CHECK_FALSE(session.Identity());
  CHECK_FALSE(session.Online(kStart));
}

TEST_CASE("PX4 Basic ID核验之前不能写入遥测或使用其他身份") {
  rtk::Px4Session session;
  CHECK_FALSE(session.Observe(BasicId(), kStart));
  session.Observe(Heartbeat(), kStart);
  CHECK_FALSE(session.Observe(Gps(), kStart));
  CHECK_FALSE(session.Observe(BasicId("wrong/system", 41), kStart));
  CHECK_FALSE(session.Observe(BasicId("bad/id"), kStart));
  CHECK_FALSE(session.Observe(BasicId(""), kStart));
  CHECK_FALSE(session.Snapshot().gps_raw_int);
  CHECK_FALSE(session.Identity());
  CHECK(session.Observe(BasicId(), kStart + 10ms));
  REQUIRE(session.Identity());
  CHECK(*session.Identity() == "PX4-EXISTING-ID");
  CHECK(session.Online(kStart + 10ms));
  CHECK(session.Observe(Gps(), kStart + 20ms));
  const auto snapshot = session.Snapshot();
  REQUIRE(snapshot.gps_raw_int);
  CHECK(snapshot.gps_raw_int->lat == 234567890);
  CHECK(snapshot.gps_raw_int->fix_type == 6);
  CHECK(snapshot.device_id == session.Identity());
  CHECK(snapshot.device_type == device::Type::kFlightController);
  CHECK_FALSE(snapshot.dcdw_label);
  CHECK_FALSE(snapshot.product);
}

TEST_CASE("同system的RemoteID组件可以提供Basic ID但其他system不能污染PX4遥测") {
  rtk::Px4Session session;
  session.Observe(Heartbeat(), kStart);
  CHECK(session.Observe(BasicId("PX4-EXISTING-ID", 42, MAV_COMP_ID_ODID_TXRX_1), kStart));
  CHECK_FALSE(session.Observe(Heartbeat(43), kStart + 1s));
  CHECK_FALSE(session.Observe(Gps(43), kStart + 1s));
  CHECK_FALSE(session.Observe(Gps(42, MAV_COMP_ID_MISSIONPLANNER), kStart + 1s));
  CHECK_FALSE(session.Snapshot().gps_raw_int);
  CHECK(session.Observe(Gps(), kStart + 1s));
  REQUIRE(session.Snapshot().gps_raw_int);
  CHECK(session.Endpoint()->system_id == 42);
  CHECK(session.LastFrameAge(kStart + 1500ms) == 500ms);
}

TEST_CASE("遥测不能续命且心跳超时后必须重新取得相同Basic ID") {
  rtk::Px4Session session;
  session.Observe(Heartbeat(), kStart);
  session.Observe(BasicId(), kStart);
  session.Observe(Gps(), kStart + 4s);
  CHECK_FALSE(session.Online(kStart + 5s));
  session.Tick(kStart + 5s);
  CHECK_FALSE(session.Identity());
  CHECK_FALSE(session.Snapshot().gps_raw_int);
  CHECK_FALSE(session.Snapshot().device_id);
  CHECK_FALSE(session.Observe(BasicId(), kStart + 5s));
  session.Observe(Heartbeat(), kStart + 6s);
  CHECK_FALSE(session.Observe(Gps(), kStart + 6s));
  CHECK_FALSE(session.Identity());
  CHECK(session.Observe(BasicId(), kStart + 6s));
  CHECK(session.Online(kStart + 6s));
  CHECK_FALSE(session.Snapshot().gps_raw_int);
}

TEST_CASE("身份冲突立即清除实时状态并禁止自动改绑") {
  rtk::Px4Session session;
  session.Observe(Heartbeat(), kStart);
  session.Observe(BasicId(), kStart);
  session.Observe(Gps(), kStart);
  CHECK_FALSE(session.Observe(BasicId("PX4-OTHER-DEVICE"), kStart + 1s));
  CHECK_FALSE(session.Identity());
  CHECK_FALSE(session.Snapshot().device_id);
  CHECK_FALSE(session.Snapshot().gps_raw_int);
  CHECK_FALSE(session.Online(kStart + 1s));
  session.Observe(Heartbeat(), kStart + 2s);
  CHECK_FALSE(session.Observe(BasicId("PX4-OTHER-DEVICE"), kStart + 2s));
  CHECK_FALSE(session.Observe(Gps(), kStart + 2s));
  CHECK(session.Observe(Heartbeat(), kStart + 3s));
  CHECK(session.Observe(BasicId(), kStart + 3s));
  CHECK(session.Online(kStart + 3s));
}

TEST_CASE("物理串口断开清除实时身份但保留绑定约束") {
  rtk::Px4Session session;
  session.Observe(Heartbeat(), kStart);
  session.Observe(BasicId(), kStart);
  session.Invalidate();
  CHECK_FALSE(session.Identity());
  CHECK_FALSE(session.LastFrameAge(kStart));
  CHECK_FALSE(session.Observe(Gps(), kStart));
  CHECK_FALSE(session.Observe(Heartbeat(43), kStart));
  session.Observe(Heartbeat(), kStart);
  CHECK_FALSE(session.Observe(BasicId("OTHER"), kStart));
}

TEST_CASE("单一发送者每秒取得GCS心跳且每两秒只请求Basic ID") {
  rtk::Px4Session session(252, MAV_COMP_ID_ONBOARD_COMPUTER);
  const auto first = session.PollOutbound(kStart);
  REQUIRE(first.size() == 1);
  CHECK(first.front().msgid == MAVLINK_MSG_ID_HEARTBEAT);
  mavlink_heartbeat_t heartbeat{};
  mavlink_msg_heartbeat_decode(&first.front(), &heartbeat);
  CHECK(heartbeat.type == MAV_TYPE_GCS);
  CHECK(heartbeat.autopilot == MAV_AUTOPILOT_INVALID);
  CHECK(first.front().sysid == 252);
  CHECK(session.PollOutbound(kStart + 500ms).empty());
  session.Observe(Heartbeat(), kStart);
  const auto pending = session.PollOutbound(kStart);
  REQUIRE(pending.size() == 1);
  CHECK(pending.front().msgid == MAVLINK_MSG_ID_COMMAND_LONG);
  mavlink_command_long_t request{};
  mavlink_msg_command_long_decode(&pending.front(), &request);
  CHECK(request.command == MAV_CMD_REQUEST_MESSAGE);
  CHECK(request.param1 == MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID);
  CHECK(request.target_system == 42);
  CHECK(request.target_component == MAV_COMP_ID_AUTOPILOT1);
  CHECK(session.PollOutbound(kStart + 999ms).empty());
  const auto next = session.PollOutbound(kStart + 2s);
  REQUIRE(next.size() == 2);
  CHECK(next[0].msgid == MAVLINK_MSG_ID_HEARTBEAT);
  CHECK(next[1].msgid == MAVLINK_MSG_ID_COMMAND_LONG);
  session.Observe(BasicId(), kStart + 2s);
  const auto verified = session.PollOutbound(kStart + 4s);
  REQUIRE(verified.size() == 3);
  CHECK(verified[0].msgid == MAVLINK_MSG_ID_HEARTBEAT);
}

TEST_CASE("GPS observation separates sample freshness from frame freshness") {
  rtk::Px4Session session;
  session.Observe(Heartbeat(), kStart);
  session.Observe(BasicId(), kStart);
  auto gps = Gps();
  std::vector<std::uint8_t> wire(MAVLINK_MAX_PACKET_LEN);
  wire.resize(mavlink_msg_to_send_buffer(wire.data(), &gps));
  CHECK(session.Observe(gps, kStart, wire));
  auto observation = session.GpsObservation(kStart);
  CHECK(observation["known"] == true);
  const auto hex = observation["raw_hex"].get<std::string>();
  REQUIRE(hex.size() == wire.size() * 2);
  for (std::size_t i = 0; i < wire.size(); ++i)
    CHECK(std::stoul(hex.substr(i * 2, 2), nullptr, 16) == wire[i]);
  CHECK(observation["sample_sequence"] == 1);
  CHECK(observation["h_acc_m"].is_null());
  session.Observe(Heartbeat(), kStart + 3s);
  observation = session.GpsObservation(kStart + 3s);
  CHECK(observation["stale"] == true);
  CHECK(observation["fix_type"].is_null());
  CHECK(observation["last_fix_type"] == 6);
  session.Observe(gps, kStart + 3100ms, wire);
  CHECK(session.GpsObservation(kStart + 3100ms)["sample_sequence"] == 2);
  CHECK(session.GpsObservation(kStart + 3100ms)["stale"] == false);
  session.Invalidate();
  CHECK(session.GpsObservation(kStart + 3s)["known"] == false);
}

TEST_CASE("duplicate GPS source time refreshes frame only and histories stay bounded") {
  rtk::Px4Session session;
  session.Observe(Heartbeat(), kStart);
  session.Observe(BasicId(), kStart);
  mavlink_gps_raw_int_t data{};
  data.time_usec = 123;
  data.fix_type = 6;
  data.h_acc = 120;
  data.v_acc = UINT32_MAX;
  data.satellites_visible = 255;
  mavlink_message_t message{};
  mavlink_msg_gps_raw_int_encode(42, 1, &message, &data);
  session.Observe(message, kStart);
  CHECK(session.GpsObservation(kStart)["h_acc_m"].get<double>() == doctest::Approx(0.12));
  CHECK(session.GpsObservation(kStart)["v_acc_m"].is_null());
  CHECK(session.GpsObservation(kStart)["satellites_visible"].is_null());
  session.Observe(Heartbeat(), kStart + 3s);
  data.fix_type = 5;
  mavlink_msg_gps_raw_int_encode(42, 1, &message, &data);
  session.Observe(message, kStart + 3s);
  const auto frozen = session.GpsObservation(kStart + 3s);
  CHECK(frozen["sample_sequence"] == 1);
  CHECK(frozen["frame_age_ms"] == 0);
  CHECK(frozen["sample_age_ms"] == 3000);
  CHECK(frozen["stale"] == true);
  CHECK(frozen["last_fix_type"] == 6);
  for (int i = 0; i < 20; ++i) {
    data.time_usec++;
    data.fix_type = i % 2 == 0 ? 5 : 6;
    mavlink_msg_gps_raw_int_encode(42, 1, &message, &data);
    session.Observe(message, kStart + 3100ms + std::chrono::milliseconds(i));
  }
  const auto history = session.GpsObservation(kStart + 3200ms);
  CHECK(history["transitions"].size() == 16);
  CHECK(history["transitions_evicted"] == 5);
  CHECK(history["sample_sequence"] == 21);
  CHECK(history["transitions"].back()["sample_sequence"] == 21);
  session.Observe(Gps(43), kStart + 3300ms);
  CHECK(session.GpsObservation(kStart + 3300ms)["sample_sequence"] == 21);
  session.Observe(BasicId("OTHER"), kStart + 3400ms);
  CHECK(session.GpsObservation(kStart + 3400ms)["known"] == false);
}

TEST_CASE("verified connection requests GPS rate and firmware once and accepts actual version") {
  rtk::Px4Session session;
  session.Observe(Heartbeat(), kStart);
  session.Observe(BasicId(), kStart);
  CHECK(session.FirmwareObservation().is_null());
  const auto outbound = session.PollOutbound(kStart);
  REQUIRE(outbound.size() == 3);
  mavlink_command_long_t interval{}, request{};
  mavlink_msg_command_long_decode(&outbound[1], &interval);
  mavlink_msg_command_long_decode(&outbound[2], &request);
  CHECK(interval.command == MAV_CMD_SET_MESSAGE_INTERVAL);
  CHECK(interval.param1 == MAVLINK_MSG_ID_GPS_RAW_INT);
  CHECK(interval.param2 == 1000000);
  CHECK(interval.target_system == 42);
  CHECK(interval.target_component == 1);
  CHECK(request.command == MAV_CMD_REQUEST_MESSAGE);
  CHECK(request.param1 == MAVLINK_MSG_ID_AUTOPILOT_VERSION);
  CHECK(session.PollOutbound(kStart + 10ms).empty());
  mavlink_autopilot_version_t version{};
  version.flight_sw_version = 0x01020304;
  mavlink_message_t message{};
  mavlink_msg_autopilot_version_encode(43, 1, &message, &version);
  CHECK_FALSE(session.Observe(message, kStart));
  CHECK(session.FirmwareObservation().is_null());
  mavlink_msg_autopilot_version_encode(42, 1, &message, &version);
  CHECK(session.Observe(message, kStart));
  CHECK(session.FirmwareObservation()["flight_sw_version"] == 0x01020304);
  session.Invalidate();
  CHECK(session.FirmwareObservation().is_null());
  session.Observe(Heartbeat(), kStart + 20ms);
  session.Observe(BasicId(), kStart + 20ms);
  CHECK(session.PollOutbound(kStart + 20ms).size() == 2);
}

TEST_CASE("duplicate GPS epoch retains the accepted wire and generic GPS measurement") {
  rtk::Px4Session session;
  session.Observe(Heartbeat(), kStart); session.Observe(BasicId(), kStart);
  mavlink_gps_raw_int_t sample{};
  sample.time_usec = 123; sample.fix_type = 5; sample.lat = 234567890;
  mavlink_message_t message{};
  mavlink_msg_gps_raw_int_encode(42, 1, &message, &sample);
  std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> original{}, duplicate{};
  const auto length = mavlink_msg_to_send_buffer(original.data(), &message);
  session.Observe(message, kStart, std::span(original).first(length));
  const auto raw = session.GpsObservation(kStart)["raw_hex"];
  sample.fix_type = 6; sample.lat = 100000000;
  mavlink_msg_gps_raw_int_encode(42, 1, &message, &sample);
  const auto duplicate_length = mavlink_msg_to_send_buffer(duplicate.data(), &message);
  session.Observe(message, kStart + 1s, std::span(duplicate).first(duplicate_length));
  const auto observation = session.GpsObservation(kStart + 1s);
  CHECK(observation["raw_hex"] == raw);
  CHECK(observation["fix_type"] == 5);
  CHECK(observation["sample_sequence"] == 1);
  CHECK(observation["sample_age_ms"] == 1000);
  CHECK(observation["frame_age_ms"] == 0);
  const auto snapshot = session.Snapshot();
  REQUIRE(snapshot.gps_raw_int);
  CHECK(snapshot.gps_raw_int->fix_type == 5);
  CHECK(snapshot.gps_raw_int->lat == 234567890);
  const auto hex = observation["raw_hex"].get<std::string>();
  REQUIRE(hex.size() == length * 2);
  for (std::size_t i = 0; i < length; ++i)
    CHECK(std::stoul(hex.substr(i * 2, 2), nullptr, 16) == original[i]);
}
