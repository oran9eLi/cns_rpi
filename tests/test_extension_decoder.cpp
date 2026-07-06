#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>

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
