#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstring>

#include "cellular/cellular_snapshot.hpp"
#include "payload/json_serializer.hpp"

TEST_CASE("空TelemetryState只输出identity.school_name其余顶层key都不存在") {
  state::TelemetryState state{};

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("identity"));
  CHECK(json["identity"]["school_name"] == "NNUTC");
  CHECK_FALSE(json["identity"].contains("vendor_id"));
  CHECK_FALSE(json["identity"].contains("dcdw_label"));
  CHECK_FALSE(json["identity"].contains("rpi_serial"));
  CHECK_FALSE(json.contains("telemetry"));
  CHECK_FALSE(json.contains("modules"));
  CHECK_FALSE(json.contains("alarms"));
  CHECK_FALSE(json.contains("logs"));
  CHECK_FALSE(json.contains("drone_id"));
}

TEST_CASE("5G状态快照写入telemetry且不公开内部诊断") {
  state::TelemetryState state{};
  cellular::StatusSnapshot cellular_status;
  cellular_status.present = true;
  cellular_status.link_state = cellular::LinkState::kDegraded;
  cellular_status.operator_name = "China Mobile";
  cellular_status.rsrp_dbm = -87;
  cellular_status.diagnostics = {
      .interface_present = true,
      .carrier_up = true,
      .has_ip_address = true,
      .has_default_route = true,
  };

  const auto json = payload::ToJson(state, "NNUTC", cellular_status);

  REQUIRE(json["telemetry"].contains("cellular_5g"));
  const auto& cellular_json = json["telemetry"]["cellular_5g"];
  CHECK(cellular_json["link_state"] == "DEGRADED");
  CHECK(cellular_json["operator"] == "China Mobile");
  CHECK(cellular_json["rsrp_dbm"] == -87);
  CHECK_FALSE(cellular_json.contains("diagnostics"));
}

TEST_CASE("identity三个可选字段各自独立按需省略") {
  state::TelemetryState state{};
  state.vendor_id = "DCDWCNS1ABCDEFGHIJKL";

  auto json = payload::ToJson(state, "NNUTC");

  CHECK(json["identity"]["vendor_id"] == "DCDWCNS1ABCDEFGHIJKL");
  CHECK_FALSE(json["identity"].contains("dcdw_label"));
  CHECK_FALSE(json["identity"].contains("rpi_serial"));

  state.dcdw_label = "DCDW-007";
  state.rpi_serial = "100000001234abcd";
  auto json2 = payload::ToJson(state, "NNUTC");

  CHECK(json2["identity"]["dcdw_label"] == "DCDW-007");
  CHECK(json2["identity"]["rpi_serial"] == "100000001234abcd");
}

TEST_CASE("heartbeat字段按原始数字透传,未收到时telemetry.heartbeat不存在") {
  state::TelemetryState state{};
  mavlink_heartbeat_t hb{};
  hb.custom_mode = 0;
  hb.type = 2;
  hb.autopilot = 12;
  hb.base_mode = 81;
  hb.system_status = 4;
  hb.mavlink_version = 3;
  state.heartbeat = hb;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("telemetry"));
  REQUIRE(json["telemetry"].contains("heartbeat"));
  CHECK(json["telemetry"]["heartbeat"]["type"] == 2);
  CHECK(json["telemetry"]["heartbeat"]["autopilot"] == 12);
  CHECK(json["telemetry"]["heartbeat"]["base_mode"] == 81);
  CHECK(json["telemetry"]["heartbeat"]["system_status"] == 4);
  CHECK(json["telemetry"]["heartbeat"]["mavlink_version"] == 3);
  CHECK_FALSE(json["telemetry"].contains("attitude"));
}

TEST_CASE("attitude弧度转角度") {
  state::TelemetryState state{};
  mavlink_attitude_t att{};
  att.time_boot_ms = 123456;
  att.roll = 1.0F;
  att.pitch = -0.5F;
  att.yaw = 0.0F;
  att.rollspeed = 0.1F;
  att.pitchspeed = -0.1F;
  att.yawspeed = 0.0F;
  state.attitude = att;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json["telemetry"].contains("attitude"));
  CHECK(json["telemetry"]["attitude"]["time_boot_ms"] == 123456);
  CHECK(json["telemetry"]["attitude"]["roll"].get<double>() == doctest::Approx(57.29578));
  CHECK(json["telemetry"]["attitude"]["pitch"].get<double>() == doctest::Approx(-28.64789));
}

TEST_CASE("gps字段换算,eph/epv命中哨兵值时输出null,vel/cog/yaw等字段不输出") {
  state::TelemetryState state{};
  mavlink_gps_raw_int_t gps{};
  gps.time_usec = 1720000000000000ULL;
  gps.lat = 399042000;
  gps.lon = 1164074000;
  gps.alt = 43500;
  gps.eph = 65535;  // UINT16_MAX -> null
  gps.epv = 150;
  gps.vel = 500;
  gps.cog = 9000;
  gps.fix_type = 3;
  gps.satellites_visible = 14;
  gps.alt_ellipsoid = 21200;
  gps.h_acc = 1100;
  gps.v_acc = 1800;
  gps.vel_acc = 300;
  gps.hdg_acc = 500;
  gps.yaw = 0;
  state.gps_raw_int = gps;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["telemetry"]["gps"];

  CHECK(out["time_usec"] == 1720000000000000ULL);
  CHECK(out["lat"].get<double>() == doctest::Approx(39.9042));
  CHECK(out["lon"].get<double>() == doctest::Approx(116.4074));
  CHECK(out["alt"].get<double>() == doctest::Approx(43.5));
  CHECK(out["alt_ellipsoid"].get<double>() == doctest::Approx(21.2));
  CHECK(out["eph"].is_null());
  CHECK(out["epv"] == 150);
  CHECK(out["fix_type"] == 3);
  CHECK(out["satellites_visible"] == 14);
  CHECK(out["h_acc"].get<double>() == doctest::Approx(1.1));
  CHECK(out["v_acc"].get<double>() == doctest::Approx(1.8));
  CHECK_FALSE(out.contains("vel"));
  CHECK_FALSE(out.contains("cog"));
  CHECK_FALSE(out.contains("yaw"));
  CHECK_FALSE(out.contains("vel_acc"));
  CHECK_FALSE(out.contains("hdg_acc"));
}

TEST_CASE("global_position字段换算,vx/vy/vz/relative_alt不输出") {
  state::TelemetryState state{};
  mavlink_global_position_int_t pos{};
  pos.time_boot_ms = 123456;
  pos.lat = 399042000;
  pos.lon = 1164074000;
  pos.alt = 43500;
  pos.relative_alt = 10000;
  pos.vx = 100;
  pos.vy = 200;
  pos.vz = -50;
  pos.hdg = 8750;
  state.global_position_int = pos;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["telemetry"]["global_position"];

  CHECK(out["time_boot_ms"] == 123456);
  CHECK(out["lat"].get<double>() == doctest::Approx(39.9042));
  CHECK(out["alt"].get<double>() == doctest::Approx(43.5));
  CHECK(out["hdg"].get<double>() == doctest::Approx(87.5));
  CHECK_FALSE(out.contains("vx"));
  CHECK_FALSE(out.contains("vy"));
  CHECK_FALSE(out.contains("vz"));
  CHECK_FALSE(out.contains("relative_alt"));
}

TEST_CASE("sys_status字段换算,current_battery命中哨兵值-1时输出null") {
  state::TelemetryState state{};
  mavlink_sys_status_t sys{};
  sys.onboard_control_sensors_present = 1483;
  sys.onboard_control_sensors_enabled = 1483;
  sys.onboard_control_sensors_health = 1483;
  sys.load = 235;
  sys.voltage_battery = 12600;
  sys.current_battery = -1;
  sys.drop_rate_comm = 1;
  sys.errors_comm = 0;
  sys.errors_count1 = 0;
  sys.errors_count2 = 0;
  sys.errors_count3 = 0;
  sys.errors_count4 = 0;
  sys.battery_remaining = 78;
  sys.onboard_control_sensors_present_extended = 0;
  sys.onboard_control_sensors_enabled_extended = 0;
  sys.onboard_control_sensors_health_extended = 0;
  state.sys_status = sys;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["telemetry"]["sys_status"];

  CHECK(out["onboard_control_sensors_present"] == 1483);
  CHECK(out["load"].get<double>() == doctest::Approx(23.5));
  CHECK(out["voltage_battery"].get<double>() == doctest::Approx(12.6));
  CHECK(out["current_battery"].is_null());
  CHECK(out["drop_rate_comm"].get<double>() == doctest::Approx(0.1));
  CHECK(out["battery_remaining"] == 78);
}

TEST_CASE("battery字段换算含哨兵值,只收到battery_status[0]时battery2不存在") {
  state::TelemetryState state{};
  mavlink_battery_status_t bs{};
  bs.current_consumed = 1520;
  bs.energy_consumed = 185;  // hJ -> J: *100 = 18500
  bs.temperature = 2850;
  std::uint16_t voltages[10] = {4150, 4140, 4150, 4130, 65535, 65535, 65535, 65535, 65535, 65535};
  std::memcpy(bs.voltages, voltages, sizeof(voltages));
  bs.current_battery = 325;
  bs.id = 0;
  bs.battery_function = 1;
  bs.type = 1;
  bs.battery_remaining = 78;
  bs.time_remaining = 3600;
  bs.charge_state = 2;
  std::uint16_t voltages_ext[4] = {0, 0, 0, 0};
  std::memcpy(bs.voltages_ext, voltages_ext, sizeof(voltages_ext));
  bs.mode = 0;
  bs.fault_bitmask = 0;
  state.battery_status[0] = bs;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["telemetry"]["battery"];

  CHECK(out["current_consumed"] == 1520);
  CHECK(out["energy_consumed"].get<double>() == doctest::Approx(18500.0));
  CHECK(out["temperature"].get<double>() == doctest::Approx(28.5));
  CHECK(out["voltages"][0].get<double>() == doctest::Approx(4.15));
  CHECK(out["voltages"][4].is_null());
  CHECK(out["current_battery"].get<double>() == doctest::Approx(3.25));
  CHECK(out["id"] == 0);
  CHECK(out["battery_remaining"] == 78);
  CHECK(out["voltages_ext"][0].is_null());
  CHECK_FALSE(json["telemetry"].contains("battery2"));
}

TEST_CASE("battery2跟battery用同一套规则,来自battery_status[1]") {
  state::TelemetryState state{};
  mavlink_battery_status_t bs{};
  bs.current_battery = -1;  // 哨兵值 -> null
  bs.battery_remaining = -1;  // 哨兵值 -> null
  bs.id = 1;
  std::uint16_t voltages[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  std::memcpy(bs.voltages, voltages, sizeof(voltages));
  std::uint16_t voltages_ext[4] = {0, 0, 0, 0};
  std::memcpy(bs.voltages_ext, voltages_ext, sizeof(voltages_ext));
  state.battery_status[1] = bs;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["telemetry"]["battery2"];

  CHECK(out["id"] == 1);
  CHECK(out["current_battery"].is_null());
  CHECK(out["battery_remaining"].is_null());
  CHECK_FALSE(json["telemetry"].contains("battery"));
}

TEST_CASE("pressure字段换算,press_abs/press_diff已是hPa直接透传") {
  state::TelemetryState state{};
  mavlink_scaled_pressure_t p{};
  p.time_boot_ms = 123456;
  p.press_abs = 1013.25F;
  p.press_diff = 0.02F;
  p.temperature = 2650;
  p.temperature_press_diff = 2650;
  state.scaled_pressure = p;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["telemetry"]["pressure"];

  CHECK(out["time_boot_ms"] == 123456);
  CHECK(out["press_abs"].get<double>() == doctest::Approx(1013.25));
  CHECK(out["press_diff"].get<double>() == doctest::Approx(0.02));
  CHECK(out["temperature"].get<double>() == doctest::Approx(26.5));
  CHECK(out["temperature_press_diff"].get<double>() == doctest::Approx(26.5));
}

TEST_CASE("gnss_sat/humidity/motor/lora/remote_id自定义字段") {
  state::TelemetryState state{};
  state.gnss_sat = state::GnssSat{9, 8, 7, 6};
  state.gnss_utc = state::GnssUtc{260716, 45296};
  state.env_humidity = state::EnvHumidity{535};
  state.motor_pwm = state::MotorPwm{{45, 45, 50, 50}, true, 60};
  state.motor_pulse = state::MotorPulse{{1000, 1250, 1500, 2000}, 123456000ULL};
  state.lora_status = state::LoraStatus{15, 9, true, 2};  // link_state=2 -> "ONLINE"
  state.lora_counters = state::LoraCounters{12, 3456, 34, 4567};
  state.remote_id_status = state::RemoteIdStatus{120, 0, 987654};

  auto json = payload::ToJson(state, "NNUTC");
  const auto& t = json["telemetry"];

  CHECK(t["gnss_sat"]["gps_visible"] == 9);
  CHECK(t["gnss_sat"]["beidou_used"] == 6);
  CHECK(t["gnss_time"]["date_yymmdd"] == 260716);
  CHECK(t["gnss_time"]["seconds_of_day"] == 45296);
  CHECK(t["gnss_time"]["date"] == "2026-07-16");
  CHECK(t["gnss_time"]["time"] == "12:34:56");
  CHECK(t["humidity"]["humidity_percent"].get<double>() == doctest::Approx(53.5));
  CHECK(t["motor"]["duty_percent"] == std::vector<int>{45, 45, 50, 50});
  CHECK(t["motor"]["run_state"] == true);
  CHECK(t["motor"]["speed_level"] == 60);
  CHECK(t["motor"]["pwm_us"] == std::vector<int>{1000, 1250, 1500, 2000});
  CHECK(t["motor"]["pwm_time_usec"] == 123456000);
  CHECK(t["lora"]["loss_rate_percent"].get<double>() == doctest::Approx(1.5));
  CHECK(t["lora"]["node_id"] == 9);
  CHECK(t["lora"]["present"] == true);
  CHECK(t["lora"]["link_state"] == "ONLINE");
  CHECK(t["lora"]["tx_frame_count"] == 12);
  CHECK(t["lora"]["tx_last_ms"] == 3456);
  CHECK(t["lora"]["rx_frame_count"] == 34);
  CHECK(t["lora"]["rx_last_ms"] == 4567);
  CHECK(t["remote_id"]["location_count"] == 120);
  CHECK(t["remote_id"]["last_success_ms"] == 987654);
}

TEST_CASE("modules数组:14个模块的name/status,未收到module_status时modules不存在") {
  state::TelemetryState empty{};
  auto empty_json = payload::ToJson(empty, "NNUTC");
  CHECK_FALSE(empty_json.contains("modules"));

  state::TelemetryState state{};
  std::array<std::uint8_t, 14> mods{};
  mods.fill(0);
  mods[0] = 2;  // GNSS=ONLINE
  mods[4] = 3;  // LORA=DEGRADED
  state.module_status = mods;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("modules"));
  REQUIRE(json["modules"].size() == 14);
  CHECK(json["modules"][0]["name"] == "GNSS");
  CHECK(json["modules"][0]["status"] == "ONLINE");
  CHECK(json["modules"][4]["name"] == "LORA");
  CHECK(json["modules"][4]["status"] == "DEGRADED");
  CHECK(json["modules"][1]["name"] == "IMU");
  CHECK(json["modules"][1]["status"] == "UNINITIALIZED");  // 零初始化占位，合法语义
  CHECK(json["modules"][13]["name"] == "BUSINESS");
}

TEST_CASE("alarms按active_count截断,未收到时不存在") {
  state::TelemetryState empty{};
  CHECK_FALSE(payload::ToJson(empty, "NNUTC").contains("alarms"));

  state::TelemetryState state{};
  state::AlarmTable table{};
  table.ver = 1;
  table.active_count = 2;
  table.entries[0] = state::AlarmEntry{4, 1032, 2, true, 15};
  table.entries[1] = state::AlarmEntry{9, 2004, 1, false, 320};
  state.alarm_table = table;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("alarms"));
  CHECK(json["alarms"]["ver"] == 1);
  REQUIRE(json["alarms"]["entries"].size() == 2);
  CHECK(json["alarms"]["entries"][0]["source_id"] == 4);
  CHECK(json["alarms"]["entries"][0]["fault_code"] == 1032);
  CHECK(json["alarms"]["entries"][0]["active"] == true);
  CHECK(json["alarms"]["entries"][1]["age_s"] == 320);
}

TEST_CASE("logs按count截断,time格式化成HH:MM:SS,未收到时不存在") {
  state::TelemetryState empty{};
  CHECK_FALSE(payload::ToJson(empty, "NNUTC").contains("logs"));

  state::TelemetryState state{};
  state::MessageLog log{};
  log.latest_seq = 458;
  log.count = 1;
  log.entries[0] = state::LogEntry{456, 12, {14, 23, 7}, 1};
  state.message_log = log;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("logs"));
  CHECK(json["logs"]["latest_seq"] == 458);
  REQUIRE(json["logs"]["entries"].size() == 1);
  CHECK(json["logs"]["entries"][0]["sequence"] == 456);
  CHECK(json["logs"]["entries"][0]["message_id"] == 12);
  CHECK(json["logs"]["entries"][0]["time"] == "14:23:07");
  CHECK(json["logs"]["entries"][0]["severity"] == 1);
}

TEST_CASE("一键起飞传感器异常日志带服务器可读告警") {
  state::TelemetryState state{};
  state::MessageLog log{};
  log.latest_seq = 33;
  log.count = 1;
  log.entries[0] = state::LogEntry{33, 32, {12, 34, 56}, 1};
  state.message_log = log;

  const auto json = payload::ToJson(state, "NNUTC");
  const auto& entry = json["logs"]["entries"][0];
  CHECK(entry["event"] == "takeoff_sensor_failure");
  CHECK(entry["level"] == "warning");
  CHECK(entry["message"] == "姿态或环境异常，一键起飞失败");
}

TEST_CASE("drone_id.basic_id:uas_id去除尾部空字符") {
  state::TelemetryState state{};
  mavlink_open_drone_id_basic_id_t basic{};
  basic.id_type = 1;
  basic.ua_type = 2;
  std::memcpy(basic.uas_id, "DCDWCNS1AB12CD34EF56", 20);
  state.open_drone_id_basic_id = basic;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["drone_id"]["basic_id"];

  CHECK(out["id_type"] == 1);
  CHECK(out["ua_type"] == 2);
  CHECK(out["uas_id"] == "DCDWCNS1AB12CD34EF56");
}

TEST_CASE("drone_id五个子块都不输出target_system/target_component/id_or_mac") {
  state::TelemetryState state{};
  state.open_drone_id_basic_id = mavlink_open_drone_id_basic_id_t{};
  state.open_drone_id_location = mavlink_open_drone_id_location_t{};
  state.open_drone_id_system = mavlink_open_drone_id_system_t{};
  state.open_drone_id_operator_id = mavlink_open_drone_id_operator_id_t{};
  state.open_drone_id_self_id = mavlink_open_drone_id_self_id_t{};

  auto json = payload::ToJson(state, "NNUTC");
  const auto& d = json["drone_id"];

  for (const char* block : {"basic_id", "location", "system", "operator_id", "self_id"}) {
    CHECK_FALSE(d[block].contains("target_system"));
    CHECK_FALSE(d[block].contains("target_component"));
    CHECK_FALSE(d[block].contains("id_or_mac"));
  }
}

TEST_CASE("drone_id.location:altitude哨兵值-1000转null,speed/direction/height等字段不输出") {
  state::TelemetryState state{};
  mavlink_open_drone_id_location_t loc{};
  loc.latitude = 399042000;
  loc.longitude = 1164074000;
  loc.altitude_barometric = -1000.0F;  // 哨兵值 -> null
  loc.altitude_geodetic = 44.8F;
  loc.timestamp = 1234.5F;
  loc.status = 2;
  loc.horizontal_accuracy = 4;
  loc.vertical_accuracy = 4;
  loc.barometer_accuracy = 3;
  loc.timestamp_accuracy = 2;
  state.open_drone_id_location = loc;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["drone_id"]["location"];

  CHECK(out["latitude"].get<double>() == doctest::Approx(39.9042));
  CHECK(out["altitude_barometric"].is_null());
  CHECK(out["altitude_geodetic"].get<double>() == doctest::Approx(44.8));
  CHECK(out["timestamp"].get<double>() == doctest::Approx(1234.5));
  CHECK(out["status"] == 2);
  CHECK_FALSE(out.contains("speed_horizontal"));
  CHECK_FALSE(out.contains("speed_vertical"));
  CHECK_FALSE(out.contains("direction"));
  CHECK_FALSE(out.contains("height"));
  CHECK_FALSE(out.contains("height_reference"));
  CHECK_FALSE(out.contains("speed_accuracy"));
}

TEST_CASE("drone_id.system:operator_altitude_geo哨兵值,area_*字段不输出") {
  state::TelemetryState state{};
  mavlink_open_drone_id_system_t sys{};
  sys.operator_latitude = 399050000;
  sys.operator_longitude = 1164080000;
  sys.operator_altitude_geo = -1000.0F;  // 哨兵值 -> null
  sys.timestamp = 233366400;
  sys.operator_location_type = 0;
  sys.classification_type = 0;
  sys.category_eu = 0;
  sys.class_eu = 0;
  state.open_drone_id_system = sys;

  auto json = payload::ToJson(state, "NNUTC");
  const auto& out = json["drone_id"]["system"];

  CHECK(out["operator_latitude"].get<double>() == doctest::Approx(39.905));
  CHECK(out["operator_altitude_geo"].is_null());
  CHECK(out["timestamp"] == 233366400);
  CHECK_FALSE(out.contains("area_ceiling"));
  CHECK_FALSE(out.contains("area_floor"));
  CHECK_FALSE(out.contains("area_count"));
  CHECK_FALSE(out.contains("area_radius"));
}

TEST_CASE("drone_id.operator_id/self_id:文本字段去除尾部空字符") {
  state::TelemetryState state{};
  mavlink_open_drone_id_operator_id_t op{};
  op.operator_id_type = 0;
  std::memset(op.operator_id, 0, sizeof(op.operator_id));
  std::memcpy(op.operator_id, "CAAB1234567890", 14);
  state.open_drone_id_operator_id = op;

  mavlink_open_drone_id_self_id_t self{};
  self.description_type = 0;
  std::memset(self.description, 0, sizeof(self.description));
  std::memcpy(self.description, "CNS-RPI training kit", 21);
  state.open_drone_id_self_id = self;

  auto json = payload::ToJson(state, "NNUTC");

  CHECK(json["drone_id"]["operator_id"]["operator_id"] == "CAAB1234567890");
  CHECK(json["drone_id"]["self_id"]["description"] == "CNS-RPI training kit");
}

TEST_CASE("全部字段同时填充,顶层结构完整,alarms/logs按截断数正确") {
  state::TelemetryState state{};

  mavlink_heartbeat_t hb{};
  hb.type = 2;
  hb.autopilot = 12;
  hb.base_mode = 81;
  hb.system_status = 4;
  hb.mavlink_version = 3;
  state.heartbeat = hb;

  mavlink_attitude_t att{};
  att.roll = 0.1F;
  state.attitude = att;

  mavlink_gps_raw_int_t gps{};
  gps.lat = 399042000;
  gps.eph = 120;
  gps.epv = 150;
  state.gps_raw_int = gps;

  mavlink_global_position_int_t pos{};
  pos.hdg = 8750;
  state.global_position_int = pos;

  mavlink_sys_status_t sys{};
  sys.current_battery = -1;
  state.sys_status = sys;

  mavlink_battery_status_t bs0{};
  bs0.id = 0;
  bs0.current_battery = 325;
  std::uint16_t voltages_ext_nonzero[4] = {3700, 0, 0, 0};
  std::memcpy(bs0.voltages_ext, voltages_ext_nonzero, sizeof(voltages_ext_nonzero));
  state.battery_status[0] = bs0;

  mavlink_battery_status_t bs1{};
  bs1.id = 1;
  state.battery_status[1] = bs1;

  mavlink_scaled_pressure_t pressure{};
  pressure.press_abs = 1013.25F;
  state.scaled_pressure = pressure;

  state.gnss_sat = state::GnssSat{9, 8, 7, 6};
  state.gnss_utc = state::GnssUtc{260716, 45296};
  state.env_humidity = state::EnvHumidity{535};
  state.motor_pwm = state::MotorPwm{{45, 45, 50, 50}, true, 60};
  state.motor_pulse = state::MotorPulse{{1000, 1250, 1500, 2000}, 123456000ULL};
  state.lora_status = state::LoraStatus{15, 9, true, 2};
  state.lora_counters = state::LoraCounters{12, 3456, 34, 4567};
  state.remote_id_status = state::RemoteIdStatus{120, 0, 987654};

  std::array<std::uint8_t, 14> mods{};
  mods.fill(2);
  state.module_status = mods;

  state::AlarmTable alarms{};
  alarms.ver = 1;
  alarms.active_count = 1;
  alarms.entries[0] = state::AlarmEntry{4, 1032, 2, true, 15};
  state.alarm_table = alarms;

  state::MessageLog logs{};
  logs.latest_seq = 1;
  logs.count = 1;
  logs.entries[0] = state::LogEntry{1, 1, {0, 0, 1}, 0};
  state.message_log = logs;

  mavlink_open_drone_id_basic_id_t basic{};
  std::memcpy(basic.uas_id, "DCDWCNS1AB12CD34EF56", 20);
  state.open_drone_id_basic_id = basic;

  mavlink_open_drone_id_location_t loc{};
  loc.altitude_barometric = 45.2F;
  state.open_drone_id_location = loc;

  mavlink_open_drone_id_system_t odsys{};
  odsys.operator_altitude_geo = 45.0F;
  state.open_drone_id_system = odsys;

  mavlink_open_drone_id_operator_id_t op{};
  std::memcpy(op.operator_id, "CAAB1234567890", 14);
  state.open_drone_id_operator_id = op;

  mavlink_open_drone_id_self_id_t self{};
  std::memcpy(self.description, "CNS-RPI training kit", 21);
  state.open_drone_id_self_id = self;

  auto json = payload::ToJson(state, "NNUTC");

  REQUIRE(json.contains("identity"));
  REQUIRE(json.contains("telemetry"));
  REQUIRE(json.contains("modules"));
  REQUIRE(json.contains("alarms"));
  REQUIRE(json.contains("logs"));
  REQUIRE(json.contains("drone_id"));

  const auto& t = json["telemetry"];
  CHECK(t.contains("heartbeat"));
  CHECK(t.contains("attitude"));
  CHECK(t.contains("gps"));
  CHECK(t.contains("global_position"));
  CHECK(t.contains("sys_status"));
  CHECK(t.contains("battery"));
  CHECK(t.contains("battery2"));
  CHECK(t.contains("pressure"));
  CHECK(t.contains("gnss_sat"));
  CHECK(t.contains("gnss_time"));
  CHECK(t.contains("humidity"));
  CHECK(t.contains("motor"));
  CHECK(t.contains("lora"));
  CHECK(t.contains("remote_id"));

  // voltages_ext里非0槽位正确换算，0槽位是null(哨兵值)——前面Task 7的测试
  // 只覆盖了全0的情况，这里补上"部分槽位有值"的情况。
  CHECK(t["battery"]["voltages_ext"][0].get<double>() == doctest::Approx(3.7));
  CHECK(t["battery"]["voltages_ext"][1].is_null());

  REQUIRE(json["modules"].size() == 14);
  CHECK(json["alarms"]["entries"].size() == 1);
  CHECK(json["logs"]["entries"].size() == 1);
  CHECK(json["logs"]["entries"][0]["time"] == "00:00:01");

  const auto& d = json["drone_id"];
  CHECK(d["basic_id"]["uas_id"] == "DCDWCNS1AB12CD34EF56");
  CHECK(d["location"]["altitude_barometric"].get<double>() == doctest::Approx(45.2));
  CHECK(d["system"]["operator_altitude_geo"].get<double>() == doctest::Approx(45.0));
  CHECK(d["operator_id"]["operator_id"] == "CAAB1234567890");
  CHECK(d["self_id"]["description"] == "CNS-RPI training kit");
}
