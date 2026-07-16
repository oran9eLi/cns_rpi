/**
 * @file extension_decoder.cpp
 * @brief extension_decoder.hpp 的实现。
 */

#include "protocol/extension_decoder.hpp"

#include "protocol/identity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace protocol {

namespace {

constexpr std::uint16_t kAlarmTablePayloadType = 0x8001;
constexpr std::size_t kAlarmHeaderSize = 2;
constexpr std::size_t kAlarmRowSize = 7;
constexpr std::size_t kAlarmMaxRows = 14;

constexpr std::uint16_t kMessageLogPayloadType = 0x8002;
constexpr std::size_t kLogHeaderSize = 3;
constexpr std::size_t kLogEntrySize = 8;
constexpr std::size_t kLogMaxEntries = 9;

/// 从裸字节数组按小端读一个 uint16_t，TUNNEL payload 里的多字节字段都是这个格式。
std::uint16_t ReadU16LE(const std::uint8_t* data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset] | (static_cast<std::uint16_t>(data[offset + 1]) << 8));
}

bool DecodeAlarmTable(const mavlink_tunnel_t& value, state::StateStore& store) {
  if (value.payload_length < kAlarmHeaderSize) {
    return false;
  }
  state::AlarmTable table{};
  table.ver = value.payload[0];
  const std::uint8_t declared_count = value.payload[1];
  const std::size_t capacity_rows = (value.payload_length - kAlarmHeaderSize) / kAlarmRowSize;
  table.active_count = std::min({static_cast<std::size_t>(declared_count), kAlarmMaxRows, capacity_rows});

  for (std::size_t i = 0; i < table.active_count; ++i) {
    const std::size_t offset = kAlarmHeaderSize + i * kAlarmRowSize;
    state::AlarmEntry& entry = table.entries[i];
    entry.source_id = value.payload[offset];
    entry.fault_code = ReadU16LE(value.payload, offset + 1);
    entry.severity = value.payload[offset + 3];
    entry.active = value.payload[offset + 4] != 0;
    entry.age_s = ReadU16LE(value.payload, offset + 5);
  }
  store.UpdateAlarmTable(table);
  return true;
}

bool DecodeMessageLog(const mavlink_tunnel_t& value, state::StateStore& store) {
  if (value.payload_length < kLogHeaderSize) {
    return false;
  }
  state::MessageLog log{};
  log.latest_seq = ReadU16LE(value.payload, 0);
  const std::uint8_t declared_count = value.payload[2];
  const std::size_t capacity_entries = (value.payload_length - kLogHeaderSize) / kLogEntrySize;
  log.count = std::min({static_cast<std::size_t>(declared_count), kLogMaxEntries, capacity_entries});

  for (std::size_t i = 0; i < log.count; ++i) {
    const std::size_t offset = kLogHeaderSize + i * kLogEntrySize;
    state::LogEntry& entry = log.entries[i];
    entry.sequence = ReadU16LE(value.payload, offset);
    entry.message_id = ReadU16LE(value.payload, offset + 2);
    entry.time_hhmmss[0] = value.payload[offset + 4];
    entry.time_hhmmss[1] = value.payload[offset + 5];
    entry.time_hhmmss[2] = value.payload[offset + 6];
    entry.severity = value.payload[offset + 7];
  }
  store.UpdateMessageLog(log);
  return true;
}

bool DecodeTunnel(const mavlink_tunnel_t& value, state::StateStore& store) {
  switch (value.payload_type) {
    case kAlarmTablePayloadType:
      return DecodeAlarmTable(value, store);
    case kMessageLogPayloadType:
      return DecodeMessageLog(value, store);
    default:
      return false;
  }
}

bool DecodeBasicId(const mavlink_open_drone_id_basic_id_t& value, state::StateStore& store) {
  store.UpdateOpenDroneIdBasicId(value);
  store.UpdateVendorId(ExtractVendorId(value.uas_id));
  return true;
}

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
  if (name == "GNSS_SAT") {
    state::GnssSat sat{};
    sat.gps_visible = static_cast<std::uint8_t>(bits & 0xFF);
    sat.beidou_visible = static_cast<std::uint8_t>((bits >> 8) & 0xFF);
    sat.gps_used = static_cast<std::uint8_t>((bits >> 16) & 0xFF);
    sat.beidou_used = static_cast<std::uint8_t>((bits >> 24) & 0xFF);
    store.UpdateGnssSat(sat);
    return true;
  }
  if (name == "GNSSUTC") {
    state::GnssUtc utc{};
    utc.date_yyyymmdd = bits;
    utc.seconds_of_day = value.time_boot_ms;
    store.UpdateGnssUtc(utc);
    return true;
  }
  if (name == "HUMIDITY") {
    state::EnvHumidity hum{};
    hum.relative_humidity_x10 = static_cast<std::uint16_t>(bits);
    store.UpdateEnvHumidity(hum);
    return true;
  }
  if (name == "MOTOR12" || name == "MOTOR34") {
    const auto duty0 = static_cast<std::uint8_t>(bits & 0xFF);
    const auto duty1 = static_cast<std::uint8_t>((bits >> 8) & 0xFF);
    const bool run_state = ((bits >> 16) & 0x1) != 0;
    const auto speed_level = static_cast<std::uint8_t>((bits >> 24) & 0xFF);
    if (name == "MOTOR12") {
      store.UpdateMotorPwmLow(duty0, duty1, run_state, speed_level);
    } else {
      store.UpdateMotorPwmHigh(duty0, duty1, run_state, speed_level);
    }
    return true;
  }
  if (name == "LORASTAT") {
    state::LoraStatus lora{};
    lora.loss_rate_x10 = static_cast<std::uint16_t>(bits & 0xFFFF);
    lora.node_id = static_cast<std::uint8_t>((bits >> 16) & 0xFF);
    lora.present = ((bits >> 24) & 0x1) != 0;
    lora.link_state = static_cast<std::uint8_t>((bits >> 25) & 0x7);
    store.UpdateLoraStatus(lora);
    return true;
  }
  if (name == "LORATX") {
    store.UpdateLoraTxCount(bits, value.time_boot_ms);
    return true;
  }
  if (name == "LORARX") {
    store.UpdateLoraRxCount(bits, value.time_boot_ms);
    return true;
  }
  if (name == "RIDSTAT") {
    state::RemoteIdStatus rid{};
    rid.location_count = static_cast<std::uint16_t>(bits & 0xFFFF);
    rid.error_count = static_cast<std::uint16_t>((bits >> 16) & 0xFFFF);
    rid.last_success_ms = value.time_boot_ms;
    store.UpdateRemoteIdStatus(rid);
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
    case MAVLINK_MSG_ID_TUNNEL: {
      mavlink_tunnel_t decoded{};
      mavlink_msg_tunnel_decode(&msg, &decoded);
      return DecodeTunnel(decoded, store);
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID: {
      mavlink_open_drone_id_basic_id_t decoded{};
      mavlink_msg_open_drone_id_basic_id_decode(&msg, &decoded);
      return DecodeBasicId(decoded, store);
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_LOCATION: {
      mavlink_open_drone_id_location_t decoded{};
      mavlink_msg_open_drone_id_location_decode(&msg, &decoded);
      store.UpdateOpenDroneIdLocation(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SYSTEM: {
      mavlink_open_drone_id_system_t decoded{};
      mavlink_msg_open_drone_id_system_decode(&msg, &decoded);
      store.UpdateOpenDroneIdSystem(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_OPERATOR_ID: {
      mavlink_open_drone_id_operator_id_t decoded{};
      mavlink_msg_open_drone_id_operator_id_decode(&msg, &decoded);
      store.UpdateOpenDroneIdOperatorId(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SELF_ID: {
      mavlink_open_drone_id_self_id_t decoded{};
      mavlink_msg_open_drone_id_self_id_decode(&msg, &decoded);
      store.UpdateOpenDroneIdSelfId(decoded);
      return true;
    }
    default:
      return false;
  }
}

}  // namespace protocol
