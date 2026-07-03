#pragma once

/**
 * @file state_store.hpp
 * @brief 解码后的内部遥测状态，线程安全存取。
 *
 * @details
 * 只负责"存一份当前已知的最新遥测状态"，不关心谁写（protocol/telemetry_decoder
 * 的事）、谁读（V1: MQTT 发布，V2 预留: Qt 渲染/CV 管线）。存官方 MAVLink 结构体
 * 原样，不做单位换算/字段筛选——那是 payload/json_serializer（M4）的事。
 * 依赖边界：只依赖官方 mavlink/c_library_v2 头文件和标准库，不包含 uart/、mqtt/
 * 等模块头文件。
 */

#include <mutex>
#include <optional>

#include "common/mavlink.h"

namespace state {

/// 一份遥测快照：每个字段在对应消息从未被解码过之前是 std::nullopt。
struct TelemetryState {
  std::optional<mavlink_heartbeat_t> heartbeat;
  std::optional<mavlink_gps_raw_int_t> gps_raw_int;
  std::optional<mavlink_attitude_t> attitude;
  std::optional<mavlink_global_position_int_t> global_position_int;
  std::optional<mavlink_sys_status_t> sys_status;
  std::optional<mavlink_battery_status_t> battery_status;
  std::optional<mavlink_scaled_pressure_t> scaled_pressure;
};

/**
 * @brief 线程安全的 TelemetryState 存取器。
 * @details 一把锁保护整个状态（数据量小、更新频率最高约 1Hz，细粒度锁是过度
 * 设计）。`Update*()` 加锁写入对应字段；`Snapshot()` 加锁拷贝整个状态返回，
 * 调用方拿到独立副本，不持有内部锁，可以在任意线程安全地读。
 */
class StateStore {
 public:
  void UpdateHeartbeat(const mavlink_heartbeat_t& value);
  void UpdateGpsRawInt(const mavlink_gps_raw_int_t& value);
  void UpdateAttitude(const mavlink_attitude_t& value);
  void UpdateGlobalPositionInt(const mavlink_global_position_int_t& value);
  void UpdateSysStatus(const mavlink_sys_status_t& value);
  void UpdateBatteryStatus(const mavlink_battery_status_t& value);
  void UpdateScaledPressure(const mavlink_scaled_pressure_t& value);

  /// 加锁拷贝当前状态并返回，调用方拿到的是独立副本。
  TelemetryState Snapshot() const;

 private:
  mutable std::mutex mutex_;
  TelemetryState state_;
};

}  // namespace state
