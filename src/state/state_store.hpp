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

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include "common/mavlink.h"

namespace state {

/// 固件端模块总数（`Px4Lite_ModuleId_t`，见 V1设计文档.md §4.1），
/// MODSTAT0 覆盖 0-7 号，MODSTAT1 覆盖 8-13 号。
constexpr std::size_t kModuleCount = 14;

/// BAT2STAT 拆包结果：电压(mV)/电量(%)/低电压标志，原始刻度，不做单位换算。
struct Battery2Status {
  std::uint16_t voltage_mv;
  std::uint8_t percent;
  bool low_voltage;
};

/// MOTORPWM 拆包结果：最多 4 个电机的占空比(%)。
struct MotorPwm {
  std::array<std::uint8_t, 4> duty_percent;
};

/// GNSS_SAT 拆包结果：GPS/北斗可见数与使用数。
struct GnssSat {
  std::uint8_t gps_visible;
  std::uint8_t beidou_visible;
  std::uint8_t gps_used;
  std::uint8_t beidou_used;
};

/// ENVHUM 拆包结果：相对湿度 x10（原始刻度，535 表示 53.5%，不做单位换算）。
struct EnvHumidity {
  std::uint16_t relative_humidity_x10;
};

/// TUNNEL 告警表(payload_type=0x8001)单行。
struct AlarmEntry {
  std::uint8_t source_id;
  std::uint16_t fault_code;
  std::uint8_t severity;
  bool active;
  std::uint16_t age_s;
};

/// TUNNEL 告警表(payload_type=0x8001)整帧，最多 14 行。
struct AlarmTable {
  std::uint8_t ver;
  std::array<AlarmEntry, 14> entries;
  std::size_t active_count;  ///< entries 中前 active_count 项有效，其余是默认值。
};

/// TUNNEL 日志增量(payload_type=0x8002)单条。
struct LogEntry {
  std::uint16_t sequence;
  std::uint16_t message_id;
  std::array<std::uint8_t, 3> time_hhmmss;
  std::uint8_t severity;
};

/// TUNNEL 日志增量(payload_type=0x8002)整帧，最多 9 条；count=0 表示只有心跳。
struct MessageLog {
  std::uint16_t latest_seq;
  std::array<LogEntry, 9> entries;
  std::size_t count;  ///< entries 中前 count 项有效，其余是默认值。
};

/// 一份遥测快照：每个字段在对应消息从未被解码过之前是 std::nullopt。
struct TelemetryState {
  std::optional<mavlink_heartbeat_t> heartbeat;
  std::optional<mavlink_gps_raw_int_t> gps_raw_int;
  std::optional<mavlink_attitude_t> attitude;
  std::optional<mavlink_global_position_int_t> global_position_int;
  std::optional<mavlink_sys_status_t> sys_status;
  std::optional<mavlink_battery_status_t> battery_status;
  std::optional<mavlink_scaled_pressure_t> scaled_pressure;

  /// 14 个模块的状态(0-6，含义见 V1设计文档.md §4.1"模块状态枚举")，
  /// MODSTAT0 只写 0-7 号，MODSTAT1 只写 8-13 号，两条帧合并成一份数据。
  std::optional<std::array<std::uint8_t, kModuleCount>> module_status;
  std::optional<Battery2Status> battery2_status;
  std::optional<MotorPwm> motor_pwm;
  std::optional<GnssSat> gnss_sat;
  std::optional<EnvHumidity> env_humidity;
  std::optional<AlarmTable> alarm_table;
  std::optional<MessageLog> message_log;
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

  /// 只写 module_status 的 0-7 号元素(来自 MODSTAT0)。若 module_status 之前
  /// 还没有值(两条帧都还没收到过)，先把整个 14 元素数组零初始化，
  /// 再写入自己负责的这一半；8-13 号元素(若已收到过 MODSTAT1)保持不变。
  void UpdateModStatusLow(const std::array<std::uint8_t, 8>& modules0to7);
  /// 只写 module_status 的 8-13 号元素(来自 MODSTAT1)，语义同上。
  void UpdateModStatusHigh(const std::array<std::uint8_t, 6>& modules8to13);
  void UpdateBattery2Status(const Battery2Status& value);
  void UpdateMotorPwm(const MotorPwm& value);
  void UpdateGnssSat(const GnssSat& value);
  void UpdateEnvHumidity(const EnvHumidity& value);
  void UpdateAlarmTable(const AlarmTable& value);
  void UpdateMessageLog(const MessageLog& value);

  /// 加锁拷贝当前状态并返回，调用方拿到的是独立副本。
  TelemetryState Snapshot() const;

 private:
  mutable std::mutex mutex_;
  TelemetryState state_;
};

}  // namespace state
