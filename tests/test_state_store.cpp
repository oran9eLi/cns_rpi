#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstring>

#include "state/state_store.hpp"

TEST_CASE("初始状态所有字段都是nullopt") {
  state::StateStore store;

  auto snapshot = store.Snapshot();

  CHECK_FALSE(snapshot.heartbeat.has_value());
  CHECK_FALSE(snapshot.gps_raw_int.has_value());
  CHECK_FALSE(snapshot.attitude.has_value());
  CHECK_FALSE(snapshot.global_position_int.has_value());
  CHECK_FALSE(snapshot.sys_status.has_value());
  CHECK_FALSE(snapshot.battery_status.has_value());
  CHECK_FALSE(snapshot.scaled_pressure.has_value());
}

TEST_CASE("Update一个字段只影响那一个字段，其余仍是nullopt") {
  state::StateStore store;
  mavlink_heartbeat_t heartbeat{};
  heartbeat.type = 18;
  heartbeat.autopilot = 8;

  store.UpdateHeartbeat(heartbeat);
  auto snapshot = store.Snapshot();

  REQUIRE(snapshot.heartbeat.has_value());
  CHECK(snapshot.heartbeat->type == 18);
  CHECK(snapshot.heartbeat->autopilot == 8);
  CHECK_FALSE(snapshot.gps_raw_int.has_value());
  CHECK_FALSE(snapshot.attitude.has_value());
}

TEST_CASE("同一字段多次Update，Snapshot返回最新值") {
  state::StateStore store;
  mavlink_scaled_pressure_t first{};
  first.press_abs = 1000.0F;
  mavlink_scaled_pressure_t second{};
  second.press_abs = 1013.25F;

  store.UpdateScaledPressure(first);
  store.UpdateScaledPressure(second);
  auto snapshot = store.Snapshot();

  REQUIRE(snapshot.scaled_pressure.has_value());
  CHECK(snapshot.scaled_pressure->press_abs == doctest::Approx(1013.25F));
}

TEST_CASE("UpdateModStatusLow只影响0-7号模块,UpdateModStatusHigh只影响8-13号,互不覆盖") {
  state::StateStore store;
  std::array<std::uint8_t, 8> low{0, 1, 2, 3, 4, 5, 6, 7};
  std::array<std::uint8_t, 6> high{0, 1, 2, 3, 4, 5};

  store.UpdateModStatusLow(low);
  auto after_low = store.Snapshot();
  REQUIRE(after_low.module_status.has_value());
  CHECK((*after_low.module_status)[0] == 0);
  CHECK((*after_low.module_status)[7] == 7);
  CHECK((*after_low.module_status)[8] == 0);  // MODSTAT1还没到，零初始化占位

  store.UpdateModStatusHigh(high);
  auto after_high = store.Snapshot();
  REQUIRE(after_high.module_status.has_value());
  CHECK((*after_high.module_status)[0] == 0);
  CHECK((*after_high.module_status)[7] == 7);  // 之前0-7的值没被UpdateModStatusHigh覆盖
  CHECK((*after_high.module_status)[8] == 0);
  CHECK((*after_high.module_status)[13] == 5);
}

TEST_CASE("扩展遥测字段的Update各自独立,不影响其他字段") {
  state::StateStore store;
  state::Battery2Status bat2{12600, 80, false};
  state::MotorPwm pwm{{10, 20, 30, 40}};
  state::GnssSat sat{12, 8, 10, 6};
  state::EnvHumidity hum{535};

  store.UpdateBattery2Status(bat2);
  store.UpdateMotorPwm(pwm);
  store.UpdateGnssSat(sat);
  store.UpdateEnvHumidity(hum);
  auto snapshot = store.Snapshot();

  REQUIRE(snapshot.battery2_status.has_value());
  CHECK(snapshot.battery2_status->voltage_mv == 12600);
  CHECK(snapshot.battery2_status->percent == 80);
  CHECK_FALSE(snapshot.battery2_status->low_voltage);

  REQUIRE(snapshot.motor_pwm.has_value());
  CHECK(snapshot.motor_pwm->duty_percent[3] == 40);

  REQUIRE(snapshot.gnss_sat.has_value());
  CHECK(snapshot.gnss_sat->beidou_used == 6);

  REQUIRE(snapshot.env_humidity.has_value());
  CHECK(snapshot.env_humidity->relative_humidity_x10 == 535);

  CHECK_FALSE(snapshot.alarm_table.has_value());
  CHECK_FALSE(snapshot.message_log.has_value());
}

TEST_CASE("身份类字段各自独立更新,不影响其他字段") {
  state::StateStore store;
  mavlink_open_drone_id_basic_id_t basic_id{};
  basic_id.id_type = 1;
  basic_id.ua_type = 2;
  std::memcpy(basic_id.uas_id, "DCDWCNS1ABCDEFGHIJKL", 20);

  store.UpdateOpenDroneIdBasicId(basic_id);
  store.UpdateVendorId("DCDWCNS1ABCDEFGHIJKL");
  store.UpdateDcdwLabel("DCDW-007");
  store.UpdateRpiSerial("100000001234abcd");

  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.open_drone_id_basic_id.has_value());
  CHECK(snapshot.open_drone_id_basic_id->id_type == 1);
  CHECK(snapshot.open_drone_id_basic_id->ua_type == 2);
  REQUIRE(snapshot.vendor_id.has_value());
  CHECK(*snapshot.vendor_id == "DCDWCNS1ABCDEFGHIJKL");
  REQUIRE(snapshot.dcdw_label.has_value());
  CHECK(*snapshot.dcdw_label == "DCDW-007");
  REQUIRE(snapshot.rpi_serial.has_value());
  CHECK(*snapshot.rpi_serial == "100000001234abcd");
  CHECK_FALSE(snapshot.open_drone_id_location.has_value());
  CHECK_FALSE(snapshot.open_drone_id_system.has_value());
  CHECK_FALSE(snapshot.open_drone_id_operator_id.has_value());
  CHECK_FALSE(snapshot.open_drone_id_self_id.has_value());
}
