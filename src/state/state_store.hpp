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
#include <string>

#include "common/mavlink.h"

namespace state {

/// 固件端模块总数（`Px4Lite_ModuleId_t`，见 V1设计文档.md §4.1），
/// MODSTAT0 覆盖 0-7 号，MODSTAT1 覆盖 8-13 号。
constexpr std::size_t kModuleCount = 14;

/// BATTERY_STATUS.id 目前只处理电池1(id=0)/电池2(id=1)两块，超出范围的id丢弃。
constexpr std::size_t kBatteryCount = 2;

/// MOTOR12/MOTOR34 拆包结果：4 个电机的占空比(%)，外加两帧共同携带的
/// run_state/speed_level(整机状态的冗余拷贝，两帧值相同，直接覆盖式更新，
/// 不像 duty_percent 那样需要区分"自己负责哪一半")。
struct MotorPwm {
  std::array<std::uint8_t, 4> duty_percent;
  bool run_state;
  std::uint8_t speed_level;
};

struct MotorPulse {
  std::array<std::uint16_t, 4> pwm_us;
  std::uint64_t time_usec;
};

/// LORASTAT 拆包结果：LoRa 链路状态(RPi 专属，只发 USART1，不上 LoRa)。
/// link_state 跟 module_status[4](LORA 模块的粗粒度状态)语义重复，但两条帧
/// 来自不同的固件消息、独立更新，不做一致性校验——state_store 存固件发的
/// 数据原样，不做二次加工。
struct LoraStatus {
  std::uint16_t loss_rate_x10;
  std::uint8_t node_id;
  bool present;
  std::uint8_t link_state;
};

struct LoraCounters {
  std::uint32_t tx_frame_count;
  std::uint32_t tx_last_ms;
  std::uint32_t rx_frame_count;
  std::uint32_t rx_last_ms;
};

/// RIDSTAT 拆包结果：RemoteID 广播状态(RPi 专属，只发 USART1)。
/// location_count/error_count 是增量语义的计数器低16位，不是绝对值。
struct RemoteIdStatus {
  std::uint16_t location_count;
  std::uint16_t error_count;
  std::uint32_t last_success_ms;
};

/// GNSS_SAT 拆包结果：GPS/北斗可见数与使用数。
struct GnssSat {
  std::uint8_t gps_visible;
  std::uint8_t beidou_visible;
  std::uint8_t gps_used;
  std::uint8_t beidou_used;
};

struct GnssUtc {
  std::uint32_t date_yymmdd;
  std::uint32_t seconds_of_day;
};

/// HUMIDITY 拆包结果：相对湿度 x10（原始刻度，535 表示 53.5%，不做单位换算）。
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
  /// 按 BATTERY_STATUS.id 区分存储：下标0是电池1(id=0)，下标1是电池2(id=1)。
  /// 固件用不同id区分两块电池，同一坑位直接覆盖会导致两块电池数据互相打架。
  std::array<std::optional<mavlink_battery_status_t>, kBatteryCount> battery_status;
  std::optional<mavlink_scaled_pressure_t> scaled_pressure;

  /// 14 个模块的状态(0-6，含义见 V1设计文档.md §4.1"模块状态枚举")，
  /// MODSTAT0 只写 0-7 号，MODSTAT1 只写 8-13 号，两条帧合并成一份数据。
  std::optional<std::array<std::uint8_t, kModuleCount>> module_status;
  std::optional<MotorPwm> motor_pwm;
  std::optional<MotorPulse> motor_pulse;
  std::optional<GnssSat> gnss_sat;
  std::optional<GnssUtc> gnss_utc;
  std::optional<EnvHumidity> env_humidity;
  std::optional<LoraStatus> lora_status;
  std::optional<LoraCounters> lora_counters;
  std::optional<RemoteIdStatus> remote_id_status;
  std::optional<AlarmTable> alarm_table;
  std::optional<MessageLog> message_log;

  /// OPEN_DRONE_ID_* 身份帧(M3c)，官方 struct 原样存储，不做单位换算/校验。
  std::optional<mavlink_open_drone_id_basic_id_t> open_drone_id_basic_id;
  std::optional<mavlink_open_drone_id_location_t> open_drone_id_location;
  std::optional<mavlink_open_drone_id_system_t> open_drone_id_system;
  std::optional<mavlink_open_drone_id_operator_id_t> open_drone_id_operator_id;
  std::optional<mavlink_open_drone_id_self_id_t> open_drone_id_self_id;

  /// 从 OPEN_DRONE_ID_BASIC_ID.uas_id 提取的厂商唯一产品识别码，RPi 不校验/不重新计算。
  std::optional<std::string> vendor_id;
  /// 从 MAVLink 帧头 sysid 格式化的 DCDW-XXX 角色号，帧头字段，不是 payload 字段。
  std::optional<std::string> dcdw_label;
  /// RPi 本机硬件序列号(/proc/cpuinfo)，V1 过渡期权威键，跟 MAVLink 帧无关。
  std::optional<std::string> rpi_serial;
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
  /// id超出kBatteryCount范围时静默丢弃，不写入、不报错(同解码层一贯的"不认识就丢弃"风格)。
  void UpdateBatteryStatus(std::uint8_t id, const mavlink_battery_status_t& value);
  void UpdateScaledPressure(const mavlink_scaled_pressure_t& value);

  /// 只写 module_status 的 0-7 号元素(来自 MODSTAT0)。若 module_status 之前
  /// 还没有值(两条帧都还没收到过)，先把整个 14 元素数组零初始化，
  /// 再写入自己负责的这一半；8-13 号元素(若已收到过 MODSTAT1)保持不变。
  void UpdateModStatusLow(const std::array<std::uint8_t, 8>& modules0to7);
  /// 只写 module_status 的 8-13 号元素(来自 MODSTAT1)，语义同上。
  void UpdateModStatusHigh(const std::array<std::uint8_t, 6>& modules8to13);
  /// 只写 duty_percent 的 0-1 号索引(来自 MOTOR12)。若 motor_pwm 之前还没有值
  /// (两帧都还没收到过)，先把整个 struct 零初始化；run_state/speed_level 是
  /// 两帧共同的冗余拷贝，每次都直接覆盖(不需要判断"谁负责哪部分")。
  void UpdateMotorPwmLow(std::uint8_t duty0, std::uint8_t duty1, bool run_state,
                          std::uint8_t speed_level);
  /// 只写 duty_percent 的 2-3 号索引(来自 MOTOR34)，语义同上。
  void UpdateMotorPwmHigh(std::uint8_t duty2, std::uint8_t duty3, bool run_state,
                           std::uint8_t speed_level);
  void UpdateMotorPulse(const MotorPulse& value);
  void UpdateGnssSat(const GnssSat& value);
  void UpdateGnssUtc(const GnssUtc& value);
  void UpdateEnvHumidity(const EnvHumidity& value);
  void UpdateLoraStatus(const LoraStatus& value);
  void UpdateLoraTxCount(std::uint32_t tx_frame_count, std::uint32_t tx_last_ms);
  void UpdateLoraRxCount(std::uint32_t rx_frame_count, std::uint32_t rx_last_ms);
  void UpdateRemoteIdStatus(const RemoteIdStatus& value);
  void UpdateAlarmTable(const AlarmTable& value);
  void UpdateMessageLog(const MessageLog& value);
  void UpdateOpenDroneIdBasicId(const mavlink_open_drone_id_basic_id_t& value);
  void UpdateOpenDroneIdLocation(const mavlink_open_drone_id_location_t& value);
  void UpdateOpenDroneIdSystem(const mavlink_open_drone_id_system_t& value);
  void UpdateOpenDroneIdOperatorId(const mavlink_open_drone_id_operator_id_t& value);
  void UpdateOpenDroneIdSelfId(const mavlink_open_drone_id_self_id_t& value);
  void UpdateVendorId(const std::string& value);
  void UpdateDcdwLabel(const std::string& value);
  void UpdateRpiSerial(const std::string& value);

  /// 加锁拷贝当前状态并返回，调用方拿到的是独立副本。
  TelemetryState Snapshot() const;

 private:
  mutable std::mutex mutex_;
  TelemetryState state_;
};

}  // namespace state
