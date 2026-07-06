/**
 * @file extension_decoder.cpp
 * @brief extension_decoder.hpp 的实现。
 */

#include "protocol/extension_decoder.hpp"

#include <array>
#include <cstring>
#include <string_view>

namespace protocol {

namespace {

/// name 字段是 char[10]，不保证有'\0'，用 strnlen 限长取值再比较，避免越界读。
bool DecodeNamedValueInt(const mavlink_named_value_int_t& value, state::StateStore& store) {
  const std::string_view name(value.name, strnlen(value.name, sizeof(value.name)));
  const auto bits = static_cast<std::uint32_t>(value.value);

  if (name == "MODSTAT0") {
    std::array<std::uint8_t, 8> modules{};
    for (std::size_t i = 0; i < modules.size(); ++i) {
      modules[i] = static_cast<std::uint8_t>((bits >> (i * 4)) & 0xF);
    }
    store.UpdateModStatusLow(modules);
    return true;
  }
  if (name == "MODSTAT1") {
    std::array<std::uint8_t, 6> modules{};
    for (std::size_t i = 0; i < modules.size(); ++i) {
      modules[i] = static_cast<std::uint8_t>((bits >> (i * 4)) & 0xF);
    }
    store.UpdateModStatusHigh(modules);
    return true;
  }
  if (name == "BAT2STAT") {
    state::Battery2Status status{};
    status.voltage_mv = static_cast<std::uint16_t>(bits & 0xFFFF);
    status.percent = static_cast<std::uint8_t>((bits >> 16) & 0xFF);
    status.low_voltage = ((bits >> 24) & 0x1) != 0;
    store.UpdateBattery2Status(status);
    return true;
  }
  if (name == "MOTORPWM") {
    state::MotorPwm pwm{};
    for (std::size_t i = 0; i < pwm.duty_percent.size(); ++i) {
      pwm.duty_percent[i] = static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFF);
    }
    store.UpdateMotorPwm(pwm);
    return true;
  }
  if (name == "GNSS_SAT") {
    state::GnssSat sat{};
    sat.gps_visible = static_cast<std::uint8_t>(bits & 0xFF);
    sat.beidou_visible = static_cast<std::uint8_t>((bits >> 8) & 0xFF);
    sat.gps_used = static_cast<std::uint8_t>((bits >> 16) & 0xFF);
    sat.beidou_used = static_cast<std::uint8_t>((bits >> 24) & 0xFF);
    store.UpdateGnssSat(sat);
    return true;
  }
  if (name == "ENVHUM") {
    state::EnvHumidity hum{};
    hum.relative_humidity_x10 = static_cast<std::uint16_t>(bits);
    store.UpdateEnvHumidity(hum);
    return true;
  }
  return false;
}

}  // namespace

bool DecodeExtensionAndStore(const mavlink_message_t& msg, state::StateStore& store) {
  switch (msg.msgid) {
    case MAVLINK_MSG_ID_NAMED_VALUE_INT: {
      mavlink_named_value_int_t decoded{};
      mavlink_msg_named_value_int_decode(&msg, &decoded);
      return DecodeNamedValueInt(decoded, store);
    }
    default:
      return false;
  }
}

}  // namespace protocol
