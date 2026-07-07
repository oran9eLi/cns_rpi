#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>

#include <array>
#include <cstring>
#include <string_view>

#include "common/mavlink.h"
#include "protocol/extension_decoder.hpp"

namespace {
constexpr std::uint8_t kSystemId = 1;
constexpr std::uint8_t kComponentId = 1;

mavlink_message_t PackNamedValueInt(const char* name, std::int32_t value) {
  mavlink_message_t msg{};
  mavlink_msg_named_value_int_pack(kSystemId, kComponentId, &msg, /*time_boot_ms=*/1000, name,
                                    value);
  return msg;
}

mavlink_message_t PackTunnel(std::uint16_t payload_type, std::uint8_t payload_length,
                              const std::array<std::uint8_t, 128>& payload) {
  mavlink_message_t msg{};
  mavlink_msg_tunnel_pack(kSystemId, kComponentId, &msg, /*target_system=*/0,
                           /*target_component=*/0, payload_type, payload_length, payload.data());
  return msg;
}

mavlink_message_t PackBasicId(std::uint8_t id_type, std::uint8_t ua_type,
                                const std::uint8_t (&uas_id)[20]) {
  mavlink_message_t msg{};
  std::uint8_t id_or_mac[20] = {};
  mavlink_msg_open_drone_id_basic_id_pack(kSystemId, kComponentId, &msg,
                                            /*target_system=*/0, /*target_component=*/0,
                                            id_or_mac, id_type, ua_type, uas_id);
  return msg;
}

mavlink_message_t PackLocation() {
  mavlink_message_t msg{};
  std::uint8_t id_or_mac[20] = {};
  mavlink_msg_open_drone_id_location_pack(
      kSystemId, kComponentId, &msg, /*target_system=*/0, /*target_component=*/0, id_or_mac,
      /*status=*/1, /*direction=*/100, /*speed_horizontal=*/200, /*speed_vertical=*/-50,
      /*latitude=*/313000000, /*longitude=*/1213000000, /*altitude_barometric=*/10.5F,
      /*altitude_geodetic=*/11.5F, /*height_reference=*/0, /*height=*/5.0F,
      /*horizontal_accuracy=*/1, /*vertical_accuracy=*/1, /*barometer_accuracy=*/1,
      /*speed_accuracy=*/1, /*timestamp=*/123.0F, /*timestamp_accuracy=*/1);
  return msg;
}

mavlink_message_t PackSystem() {
  mavlink_message_t msg{};
  std::uint8_t id_or_mac[20] = {};
  mavlink_msg_open_drone_id_system_pack(
      kSystemId, kComponentId, &msg, /*target_system=*/0, /*target_component=*/0, id_or_mac,
      /*operator_location_type=*/0, /*classification_type=*/0, /*operator_latitude=*/313000000,
      /*operator_longitude=*/1213000000, /*area_count=*/1, /*area_radius=*/0,
      /*area_ceiling=*/-1000.0F, /*area_floor=*/-1000.0F, /*category_eu=*/0, /*class_eu=*/0,
      /*operator_altitude_geo=*/-1000.0F, /*timestamp=*/1700000000U);
  return msg;
}

mavlink_message_t PackOperatorId(const char* operator_id) {
  mavlink_message_t msg{};
  std::uint8_t id_or_mac[20] = {};
  mavlink_msg_open_drone_id_operator_id_pack(kSystemId, kComponentId, &msg,
                                               /*target_system=*/0, /*target_component=*/0,
                                               id_or_mac, /*operator_id_type=*/0, operator_id);
  return msg;
}

mavlink_message_t PackSelfId(const char* description) {
  mavlink_message_t msg{};
  std::uint8_t id_or_mac[20] = {};
  mavlink_msg_open_drone_id_self_id_pack(kSystemId, kComponentId, &msg, /*target_system=*/0,
                                           /*target_component=*/0, id_or_mac,
                                           /*description_type=*/0, description);
  return msg;
}
}  // namespace

TEST_CASE("MODSTAT0解码写入module_status的0-7号,8-13号保持零初始化") {
  constexpr std::int32_t kValue = static_cast<std::int32_t>(
      (0u << 0) | (1u << 4) | (2u << 8) | (3u << 12) | (4u << 16) | (5u << 20) | (6u << 24) |
      (7u << 28));
  mavlink_message_t msg = PackNamedValueInt("MODSTAT0", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.module_status.has_value());
  CHECK((*snapshot.module_status)[0] == 0);
  CHECK((*snapshot.module_status)[3] == 3);
  CHECK((*snapshot.module_status)[7] == 7);
  CHECK((*snapshot.module_status)[8] == 0);
}

TEST_CASE("MODSTAT1解码写入module_status的8-13号,不影响先前的0-7号") {
  constexpr std::int32_t kLowValue = static_cast<std::int32_t>(
      (0u << 0) | (1u << 4) | (2u << 8) | (3u << 12) | (4u << 16) | (5u << 20) | (6u << 24) |
      (7u << 28));
  constexpr std::int32_t kHighValue = static_cast<std::int32_t>(
      (0u << 0) | (1u << 4) | (2u << 8) | (3u << 12) | (4u << 16) | (5u << 20));
  state::StateStore store;
  protocol::DecodeExtensionAndStore(PackNamedValueInt("MODSTAT0", kLowValue), store);

  bool handled = protocol::DecodeExtensionAndStore(PackNamedValueInt("MODSTAT1", kHighValue), store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.module_status.has_value());
  CHECK((*snapshot.module_status)[7] == 7);   // 0-7号仍是MODSTAT0写入的值
  CHECK((*snapshot.module_status)[8] == 0);
  CHECK((*snapshot.module_status)[13] == 5);
}

TEST_CASE("BAT2STAT解码拆出电压/电量/低电压标志") {
  constexpr std::int32_t kValue = static_cast<std::int32_t>(10500u | (15u << 16) | (1u << 24));
  mavlink_message_t msg = PackNamedValueInt("BAT2STAT", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.battery2_status.has_value());
  CHECK(snapshot.battery2_status->voltage_mv == 10500);
  CHECK(snapshot.battery2_status->percent == 15);
  CHECK(snapshot.battery2_status->low_voltage);
}

TEST_CASE("GNSS_SAT解码拆出GPS/北斗可见数与使用数") {
  constexpr std::int32_t kValue =
      static_cast<std::int32_t>(12u | (8u << 8) | (10u << 16) | (6u << 24));
  mavlink_message_t msg = PackNamedValueInt("GNSS_SAT", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.gnss_sat.has_value());
  CHECK(snapshot.gnss_sat->gps_visible == 12);
  CHECK(snapshot.gnss_sat->beidou_visible == 8);
  CHECK(snapshot.gnss_sat->gps_used == 10);
  CHECK(snapshot.gnss_sat->beidou_used == 6);
}

TEST_CASE("HUMIDITY解码保持原始x10刻度,不做单位换算") {
  mavlink_message_t msg = PackNamedValueInt("HUMIDITY", /*value=*/535);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.env_humidity.has_value());
  CHECK(snapshot.env_humidity->relative_humidity_x10 == 535);
}

TEST_CASE("不认识的NAMED_VALUE_INT名字被安静忽略") {
  mavlink_message_t msg = PackNamedValueInt("FOOBAR", /*value=*/123);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK_FALSE(handled);
  auto snapshot = store.Snapshot();
  CHECK_FALSE(snapshot.env_humidity.has_value());
}

TEST_CASE("不认识的消息类型被安静忽略") {
  mavlink_message_t msg{};
  mavlink_msg_heartbeat_pack(kSystemId, kComponentId, &msg, /*type=*/18, /*autopilot=*/8,
                             /*base_mode=*/0, /*custom_mode=*/0, /*system_status=*/4);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK_FALSE(handled);
}

TEST_CASE("TUNNEL告警表解码出ver/active_count/每行字段") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 1;                          // ver
  payload[1] = 2;                          // active_count
  payload[2] = 3;                          // row0.source_id
  payload[3] = 0x34;
  payload[4] = 0x12;                       // row0.fault_code=0x1234 LE
  payload[5] = 2;                          // row0.severity
  payload[6] = 1;                          // row0.active=true
  payload[7] = 100;
  payload[8] = 0;                          // row0.age_s=100 LE
  payload[9] = 7;                          // row1.source_id
  payload[10] = 0x56;
  payload[11] = 0x00;                      // row1.fault_code=0x0056 LE
  payload[12] = 4;                         // row1.severity
  payload[13] = 0;                         // row1.active=false
  payload[14] = 0x88;
  payload[15] = 0x13;                      // row1.age_s=5000 LE
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x8001, /*payload_length=*/16, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.alarm_table.has_value());
  CHECK(snapshot.alarm_table->ver == 1);
  CHECK(snapshot.alarm_table->active_count == 2);
  CHECK(snapshot.alarm_table->entries[0].source_id == 3);
  CHECK(snapshot.alarm_table->entries[0].fault_code == 0x1234);
  CHECK(snapshot.alarm_table->entries[0].severity == 2);
  CHECK(snapshot.alarm_table->entries[0].active);
  CHECK(snapshot.alarm_table->entries[0].age_s == 100);
  CHECK(snapshot.alarm_table->entries[1].source_id == 7);
  CHECK(snapshot.alarm_table->entries[1].fault_code == 0x0056);
  CHECK(snapshot.alarm_table->entries[1].severity == 4);
  CHECK_FALSE(snapshot.alarm_table->entries[1].active);
  CHECK(snapshot.alarm_table->entries[1].age_s == 5000);
}

TEST_CASE("TUNNEL日志增量解码出latest_seq/count/每条字段") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 42;
  payload[1] = 0;                          // latest_seq=42 LE
  payload[2] = 1;                          // count
  payload[3] = 42;
  payload[4] = 0;                          // entry0.sequence=42 LE
  payload[5] = 7;
  payload[6] = 0;                          // entry0.message_id=7 LE
  payload[7] = 9;
  payload[8] = 30;
  payload[9] = 15;                         // entry0.time_hhmmss={9,30,15}
  payload[10] = 1;                         // entry0.severity=WARNING
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x8002, /*payload_length=*/11, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.message_log.has_value());
  CHECK(snapshot.message_log->latest_seq == 42);
  CHECK(snapshot.message_log->count == 1);
  CHECK(snapshot.message_log->entries[0].sequence == 42);
  CHECK(snapshot.message_log->entries[0].message_id == 7);
  CHECK(snapshot.message_log->entries[0].time_hhmmss[0] == 9);
  CHECK(snapshot.message_log->entries[0].time_hhmmss[1] == 30);
  CHECK(snapshot.message_log->entries[0].time_hhmmss[2] == 15);
  CHECK(snapshot.message_log->entries[0].severity == 1);
}

TEST_CASE("TUNNEL日志增量count=0时只有latest_seq有意义,不是畸形") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 100;
  payload[1] = 0;                          // latest_seq=100 LE
  payload[2] = 0;                          // count=0
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x8002, /*payload_length=*/3, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.message_log.has_value());
  CHECK(snapshot.message_log->latest_seq == 100);
  CHECK(snapshot.message_log->count == 0);
}

TEST_CASE("TUNNEL告警表声明行数超过协议上限14时被clamp到14") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 1;                          // ver
  payload[1] = 200;                        // active_count声明成200(超协议上限)
  payload[2] = 9;                          // row0.source_id(用来验证真正解析出来的行有效)
  payload[3] = 0x01;
  payload[4] = 0x00;                       // row0.fault_code=1
  payload[5] = 0;                          // row0.severity
  payload[6] = 1;                          // row0.active=true
  payload[7] = 1;
  payload[8] = 0;                          // row0.age_s=1
  // payload_length按14行的完整长度给,2+14*7=100,足够容纳,应该clamp到协议硬上限14
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x8001, /*payload_length=*/100, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.alarm_table.has_value());
  CHECK(snapshot.alarm_table->active_count == 14);
  CHECK(snapshot.alarm_table->entries[0].source_id == 9);
}

TEST_CASE("TUNNEL告警表payload_length不够声明行数时被clamp到实际能容纳的行数") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 1;                          // ver
  payload[1] = 10;                         // active_count声明成10(协议上限内)
  payload[2] = 9;                          // row0.source_id
  payload[3] = 1;
  payload[4] = 0;                          // row0.fault_code=1
  payload[5] = 0;                          // row0.severity
  payload[6] = 1;                          // row0.active=true
  payload[7] = 1;
  payload[8] = 0;                          // row0.age_s=1
  // payload_length只够2字节表头+3行(2+3*7=23),声明的10行装不下,应该clamp到3
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x8001, /*payload_length=*/23, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.alarm_table.has_value());
  CHECK(snapshot.alarm_table->active_count == 3);
  CHECK(snapshot.alarm_table->entries[0].source_id == 9);
}

TEST_CASE("TUNNEL日志增量声明条数超过协议上限9时被clamp到9") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 7;
  payload[1] = 0;                          // latest_seq=7 LE
  payload[2] = 200;                        // count声明成200(超协议上限)
  payload[3] = 1;
  payload[4] = 0;                          // entry0.sequence=1 LE
  payload[5] = 2;
  payload[6] = 0;                          // entry0.message_id=2 LE
  payload[7] = 3;
  payload[8] = 4;
  payload[9] = 5;                          // entry0.time_hhmmss={3,4,5}
  payload[10] = 6;                         // entry0.severity=6
  // payload_length按9条的完整长度给,3+9*8=75,足够容纳,应该clamp到协议硬上限9
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x8002, /*payload_length=*/75, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.message_log.has_value());
  CHECK(snapshot.message_log->latest_seq == 7);
  CHECK(snapshot.message_log->count == 9);
  CHECK(snapshot.message_log->entries[0].sequence == 1);
  CHECK(snapshot.message_log->entries[0].message_id == 2);
  CHECK(snapshot.message_log->entries[0].severity == 6);
}

TEST_CASE("TUNNEL日志增量payload_length不够声明条数时被clamp到实际能容纳的条数") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 50;
  payload[1] = 0;                          // latest_seq=50 LE
  payload[2] = 8;                          // count声明成8(协议上限内)
  payload[3] = 1;
  payload[4] = 0;                          // entry0.sequence=1 LE
  payload[5] = 2;
  payload[6] = 0;                          // entry0.message_id=2 LE
  payload[7] = 3;
  payload[8] = 4;
  payload[9] = 5;                          // entry0.time_hhmmss={3,4,5}
  payload[10] = 6;                         // entry0.severity=6
  // payload_length只够3字节表头+2条(3+2*8=19),声明的8条装不下,应该clamp到2
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x8002, /*payload_length=*/19, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.message_log.has_value());
  CHECK(snapshot.message_log->latest_seq == 50);
  CHECK(snapshot.message_log->count == 2);
  CHECK(snapshot.message_log->entries[0].sequence == 1);
}

TEST_CASE("TUNNEL payload_length小于表头长度时整帧判畸形,返回false不写入store") {
  std::array<std::uint8_t, 128> payload{};
  payload[0] = 1;
  mavlink_message_t alarm_msg = PackTunnel(/*payload_type=*/0x8001, /*payload_length=*/1, payload);
  mavlink_message_t log_msg = PackTunnel(/*payload_type=*/0x8002, /*payload_length=*/2, payload);
  state::StateStore store;

  bool alarm_handled = protocol::DecodeExtensionAndStore(alarm_msg, store);
  bool log_handled = protocol::DecodeExtensionAndStore(log_msg, store);

  CHECK_FALSE(alarm_handled);
  CHECK_FALSE(log_handled);
  auto snapshot = store.Snapshot();
  CHECK_FALSE(snapshot.alarm_table.has_value());
  CHECK_FALSE(snapshot.message_log.has_value());
}

TEST_CASE("不认识的payload_type被安静忽略") {
  std::array<std::uint8_t, 128> payload{};
  mavlink_message_t msg = PackTunnel(/*payload_type=*/0x9999, /*payload_length=*/10, payload);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK_FALSE(handled);
}

TEST_CASE("OPEN_DRONE_ID_BASIC_ID解码存储原始struct并提取vendor_id") {
  std::uint8_t uas_id[20] = {'D', 'C', 'D', 'W', 'C', 'N', 'S', '1', 'A', 'B',
                              'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K', 'L', 'M'};
  mavlink_message_t msg = PackBasicId(/*id_type=*/1, /*ua_type=*/2, uas_id);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.open_drone_id_basic_id.has_value());
  CHECK(snapshot.open_drone_id_basic_id->id_type == 1);
  CHECK(snapshot.open_drone_id_basic_id->ua_type == 2);
  REQUIRE(snapshot.vendor_id.has_value());
  CHECK(*snapshot.vendor_id == "DCDWCNS1ABCDEFGHJKLM");
}

TEST_CASE("OPEN_DRONE_ID_BASIC_ID的uas_id中间有null时vendor_id按strnlen截断") {
  std::uint8_t uas_id[20] = {'D', 'C', 'D', 'W', 'C', 'N', 'S', '1', 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  mavlink_message_t msg = PackBasicId(/*id_type=*/1, /*ua_type=*/0, uas_id);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.vendor_id.has_value());
  CHECK(*snapshot.vendor_id == "DCDWCNS1");
}

TEST_CASE("OPEN_DRONE_ID_LOCATION解码存储原始struct") {
  mavlink_message_t msg = PackLocation();
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.open_drone_id_location.has_value());
  CHECK(snapshot.open_drone_id_location->latitude == 313000000);
  CHECK(snapshot.open_drone_id_location->longitude == 1213000000);
  CHECK(snapshot.open_drone_id_location->speed_vertical == -50);
}

TEST_CASE("OPEN_DRONE_ID_SYSTEM解码存储原始struct") {
  mavlink_message_t msg = PackSystem();
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.open_drone_id_system.has_value());
  CHECK(snapshot.open_drone_id_system->operator_latitude == 313000000);
  CHECK(snapshot.open_drone_id_system->timestamp == 1700000000U);
}

TEST_CASE("OPEN_DRONE_ID_OPERATOR_ID解码存储原始struct") {
  mavlink_message_t msg = PackOperatorId("CAA123456");
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.open_drone_id_operator_id.has_value());
  CHECK(std::string_view(snapshot.open_drone_id_operator_id->operator_id,
                          strnlen(snapshot.open_drone_id_operator_id->operator_id, 20)) ==
        "CAA123456");
}

TEST_CASE("OPEN_DRONE_ID_SELF_ID解码存储原始struct") {
  mavlink_message_t msg = PackSelfId("training kit demo");
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.open_drone_id_self_id.has_value());
  CHECK(std::string_view(snapshot.open_drone_id_self_id->description,
                          strnlen(snapshot.open_drone_id_self_id->description, 23)) ==
        "training kit demo");
}

TEST_CASE("MOTOR12解码写入duty_percent的0-1号,run_state/speed_level一并写入") {
  constexpr std::int32_t kValue =
      static_cast<std::int32_t>(10u | (20u << 8) | (1u << 16) | (50u << 24));
  mavlink_message_t msg = PackNamedValueInt("MOTOR12", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.motor_pwm.has_value());
  CHECK(snapshot.motor_pwm->duty_percent[0] == 10);
  CHECK(snapshot.motor_pwm->duty_percent[1] == 20);
  CHECK(snapshot.motor_pwm->run_state);
  CHECK(snapshot.motor_pwm->speed_level == 50);
}

TEST_CASE("MOTOR34解码写入duty_percent的2-3号,不影响MOTOR12已写入的0-1号") {
  constexpr std::int32_t kLowValue =
      static_cast<std::int32_t>(10u | (20u << 8) | (1u << 16) | (50u << 24));
  constexpr std::int32_t kHighValue =
      static_cast<std::int32_t>(30u | (40u << 8) | (0u << 16) | (60u << 24));
  state::StateStore store;
  protocol::DecodeExtensionAndStore(PackNamedValueInt("MOTOR12", kLowValue), store);

  bool handled = protocol::DecodeExtensionAndStore(PackNamedValueInt("MOTOR34", kHighValue), store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.motor_pwm.has_value());
  CHECK(snapshot.motor_pwm->duty_percent[0] == 10);  // 0-1号仍是MOTOR12写入的值
  CHECK(snapshot.motor_pwm->duty_percent[1] == 20);
  CHECK(snapshot.motor_pwm->duty_percent[2] == 30);
  CHECK(snapshot.motor_pwm->duty_percent[3] == 40);
  CHECK_FALSE(snapshot.motor_pwm->run_state);  // 两帧冗余拷贝,以最新一帧为准
  CHECK(snapshot.motor_pwm->speed_level == 60);
}

TEST_CASE("LORASTAT解码拆出丢包率/节点ID/在位标志/链路状态") {
  constexpr std::int32_t kValue =
      static_cast<std::int32_t>(100u | (7u << 16) | (1u << 24) | (2u << 25));
  mavlink_message_t msg = PackNamedValueInt("LORASTAT", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.lora_status.has_value());
  CHECK(snapshot.lora_status->loss_rate_x10 == 100);
  CHECK(snapshot.lora_status->node_id == 7);
  CHECK(snapshot.lora_status->present);
  CHECK(snapshot.lora_status->link_state == 2);
}

TEST_CASE("RIDSTAT解码拆出位置广播成功计数/错误计数,time_boot_ms存入last_success_ms") {
  mavlink_message_t msg{};
  constexpr std::int32_t kValue = static_cast<std::int32_t>(50u | (3u << 16));
  mavlink_msg_named_value_int_pack(kSystemId, kComponentId, &msg, /*time_boot_ms=*/123456,
                                    "RIDSTAT", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.remote_id_status.has_value());
  CHECK(snapshot.remote_id_status->location_count == 50);
  CHECK(snapshot.remote_id_status->error_count == 3);
  CHECK(snapshot.remote_id_status->last_success_ms == 123456);
}
