#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>

#include <array>

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

TEST_CASE("MOTORPWM解码拆出4个电机占空比") {
  constexpr std::int32_t kValue =
      static_cast<std::int32_t>(10u | (20u << 8) | (30u << 16) | (40u << 24));
  mavlink_message_t msg = PackNamedValueInt("MOTORPWM", kValue);
  state::StateStore store;

  bool handled = protocol::DecodeExtensionAndStore(msg, store);

  CHECK(handled);
  auto snapshot = store.Snapshot();
  REQUIRE(snapshot.motor_pwm.has_value());
  CHECK(snapshot.motor_pwm->duty_percent[0] == 10);
  CHECK(snapshot.motor_pwm->duty_percent[1] == 20);
  CHECK(snapshot.motor_pwm->duty_percent[2] == 30);
  CHECK(snapshot.motor_pwm->duty_percent[3] == 40);
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

TEST_CASE("ENVHUM解码保持原始x10刻度,不做单位换算") {
  mavlink_message_t msg = PackNamedValueInt("ENVHUM", /*value=*/535);
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
