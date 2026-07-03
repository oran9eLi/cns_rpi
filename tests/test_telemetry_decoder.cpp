#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>

#include "common/mavlink.h"
#include "protocol/telemetry_decoder.hpp"

namespace {
constexpr std::uint8_t kSystemId = 1;
constexpr std::uint8_t kComponentId = 1;
}  // namespace

TEST_CASE("HEARTBEAT解码写入store") {
  mavlink_message_t msg{};
  mavlink_msg_heartbeat_pack(kSystemId, kComponentId, &msg, /*type=*/18, /*autopilot=*/8,
                             /*base_mode=*/0, /*custom_mode=*/0, /*system_status=*/4);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.heartbeat.has_value());
  CHECK(snapshot.heartbeat->type == 18);
  CHECK(snapshot.heartbeat->autopilot == 8);
  CHECK(snapshot.heartbeat->system_status == 4);
}

TEST_CASE("GPS_RAW_INT解码写入store") {
  mavlink_message_t msg{};
  mavlink_msg_gps_raw_int_pack(kSystemId, kComponentId, &msg,
                               /*time_usec=*/123456789ULL, /*fix_type=*/3,
                               /*lat=*/396890000, /*lon=*/1164050000, /*alt=*/50000,
                               /*eph=*/100, /*epv=*/150, /*vel=*/500, /*cog=*/9000,
                               /*satellites_visible=*/12, /*alt_ellipsoid=*/50500,
                               /*h_acc=*/1000, /*v_acc=*/2000, /*vel_acc=*/300,
                               /*hdg_acc=*/500, /*yaw=*/0);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.gps_raw_int.has_value());
  CHECK(snapshot.gps_raw_int->fix_type == 3);
  CHECK(snapshot.gps_raw_int->lat == 396890000);
  CHECK(snapshot.gps_raw_int->lon == 1164050000);
  CHECK(snapshot.gps_raw_int->satellites_visible == 12);
}

TEST_CASE("ATTITUDE解码写入store") {
  mavlink_message_t msg{};
  mavlink_msg_attitude_pack(kSystemId, kComponentId, &msg, /*time_boot_ms=*/1000,
                            /*roll=*/0.1F, /*pitch=*/0.2F, /*yaw=*/0.3F,
                            /*rollspeed=*/0.01F, /*pitchspeed=*/0.02F, /*yawspeed=*/0.03F);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.attitude.has_value());
  CHECK(snapshot.attitude->roll == doctest::Approx(0.1F));
  CHECK(snapshot.attitude->pitch == doctest::Approx(0.2F));
  CHECK(snapshot.attitude->yaw == doctest::Approx(0.3F));
}

TEST_CASE("GLOBAL_POSITION_INT解码写入store") {
  mavlink_message_t msg{};
  mavlink_msg_global_position_int_pack(kSystemId, kComponentId, &msg, /*time_boot_ms=*/1000,
                                       /*lat=*/396890000, /*lon=*/1164050000, /*alt=*/50000,
                                       /*relative_alt=*/10000, /*vx=*/100, /*vy=*/200,
                                       /*vz=*/-50, /*hdg=*/9000);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.global_position_int.has_value());
  CHECK(snapshot.global_position_int->lat == 396890000);
  CHECK(snapshot.global_position_int->relative_alt == 10000);
  CHECK(snapshot.global_position_int->hdg == 9000);
}

TEST_CASE("SYS_STATUS解码写入store") {
  mavlink_message_t msg{};
  mavlink_msg_sys_status_pack(kSystemId, kComponentId, &msg,
                              /*onboard_control_sensors_present=*/0x01,
                              /*onboard_control_sensors_enabled=*/0x01,
                              /*onboard_control_sensors_health=*/0x01,
                              /*load=*/300, /*voltage_battery=*/12600, /*current_battery=*/150,
                              /*battery_remaining=*/80, /*drop_rate_comm=*/0, /*errors_comm=*/0,
                              /*errors_count1=*/0, /*errors_count2=*/0, /*errors_count3=*/0,
                              /*errors_count4=*/0,
                              /*onboard_control_sensors_present_extended=*/0,
                              /*onboard_control_sensors_enabled_extended=*/0,
                              /*onboard_control_sensors_health_extended=*/0);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.sys_status.has_value());
  CHECK(snapshot.sys_status->voltage_battery == 12600);
  CHECK(snapshot.sys_status->battery_remaining == 80);
}

TEST_CASE("BATTERY_STATUS解码写入store") {
  mavlink_message_t msg{};
  std::uint16_t voltages[10] = {4200, 4180, 4190, 0, 0, 0, 0, 0, 0, 0};
  std::uint16_t voltages_ext[4] = {0, 0, 0, 0};
  mavlink_msg_battery_status_pack(kSystemId, kComponentId, &msg, /*id=*/0,
                                  /*battery_function=*/0, /*type=*/0, /*temperature=*/2500,
                                  voltages, /*current_battery=*/150, /*current_consumed=*/500,
                                  /*energy_consumed=*/1000, /*battery_remaining=*/80,
                                  /*time_remaining=*/3600, /*charge_state=*/0, voltages_ext,
                                  /*mode=*/0, /*fault_bitmask=*/0);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.battery_status.has_value());
  CHECK(snapshot.battery_status->voltages[0] == 4200);
  CHECK(snapshot.battery_status->battery_remaining == 80);
}

TEST_CASE("SCALED_PRESSURE解码写入store") {
  mavlink_message_t msg{};
  mavlink_msg_scaled_pressure_pack(kSystemId, kComponentId, &msg, /*time_boot_ms=*/1000,
                                   /*press_abs=*/1013.25F, /*press_diff=*/0.5F,
                                   /*temperature=*/2500, /*temperature_press_diff=*/2500);
  state::StateStore store;

  bool handled = protocol::DecodeAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.scaled_pressure.has_value());
  CHECK(snapshot.scaled_pressure->press_abs == doctest::Approx(1013.25F));
}

TEST_CASE("不认识的消息类型被安静忽略，不影响其他已有字段") {
  state::StateStore store;
  mavlink_message_t heartbeat_msg{};
  mavlink_msg_heartbeat_pack(kSystemId, kComponentId, &heartbeat_msg, /*type=*/18,
                             /*autopilot=*/8, /*base_mode=*/0, /*custom_mode=*/0,
                             /*system_status=*/4);
  protocol::DecodeAndStore(heartbeat_msg, store);

  mavlink_message_t statustext_msg{};
  mavlink_msg_statustext_pack(kSystemId, kComponentId, &statustext_msg, /*severity=*/6,
                              "test", /*id=*/0, /*chunk_seq=*/0);

  bool handled = protocol::DecodeAndStore(statustext_msg, store);

  CHECK_FALSE(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.heartbeat.has_value());
  CHECK(snapshot.heartbeat->type == 18);
  CHECK_FALSE(snapshot.gps_raw_int.has_value());
}
