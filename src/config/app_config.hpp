#pragma once

/**
 * @file app_config.hpp
 * @brief 读取 config/config.json，给各模块提供启动配置。
 *
 * @details
 * MQTT 配置按 connection/auth/topics 分组，配置加载阶段完成类型、范围和
 * topic 单段合法性校验，运行循环只消费已经验证的值。
 * 依赖边界：只使用配置与资料值类型，不包含 uart/、mqtt/ 等运行模块头文件。
 */

#include <expected>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "device_status/metadata.hpp"

namespace config {

/// 配置加载失败的原因。
enum class ConfigError {
  kFileNotFound,   ///< 文件路径打不开
  kParseError,     ///< 内容不是合法 JSON
  kMissingField,   ///< 缺少必需字段
  kInvalidValue,   ///< 字段类型错误或取值超出允许范围
};

/// 转成人可读的启动错误原因。
std::string_view ConfigErrorMessage(ConfigError error);

struct SerialConfig {
  std::string device;  ///< `auto` 或明确字符设备路径，例如 "/dev/ttyUSB0"
  int baud = 0;        ///< 波特率
};

struct TelemetryChannelConfig {
  bool enabled{true};
  std::chrono::milliseconds interval{0};
};

struct TelemetryPublishConfig {
  TelemetryChannelConfig snapshot{true, std::chrono::milliseconds(1000)};
  TelemetryChannelConfig realtime{true, std::chrono::milliseconds(100)};
};

struct MqttConnectionConfig {
  std::string host;
  int port = 0;
  int keepalive_seconds = 0;
  /// config.json 的 client_id 是产品前缀，连接时追加 "-{device_id}" 保证唯一。
  std::string client_id_prefix;
  struct ReconnectConfig {
    int delay_seconds = 0;
    int delay_max_seconds = 0;
  } reconnect;
};

struct MqttAuthConfig {
  std::string username;
  std::string password;
};

struct MqttTopicConfig {
  std::string suffix;
  int qos = 0;
};

struct MqttTopicsConfig {
  std::string topic_namespace;
  MqttTopicConfig registration;
  MqttTopicConfig telemetry_snapshot;
  MqttTopicConfig telemetry_realtime;
  MqttTopicConfig runtime_status{"runtime/status/v1", 1};
  MqttTopicConfig config_set;
  MqttTopicConfig config_ack;
  MqttTopicConfig control_set{"control/set", 2};
  MqttTopicConfig control_ack{"control/ack", 2};
};

struct MqttConfig {
  MqttConnectionConfig connection;
  MqttAuthConfig auth;
  MqttTopicsConfig topics;
};

struct LoggingConfig {
  std::string level;
  std::string file;
  std::size_t max_file_size_bytes = 0;  ///< 单个日志文件的容量上限，单位为字节。
};

struct IdentityConfig {
  std::string school_name;
  std::filesystem::path binding_file{"/var/lib/cns-rpi/device_binding.json"};
};

struct RuntimeConfig {
  std::chrono::milliseconds heartbeat_interval{0};
  std::vector<std::string> applied_command_ids;
};

struct CellularConfig {
  std::string interface_name{"usb0"};
  std::chrono::milliseconds heartbeat_interval{1000};
  std::filesystem::path status_snapshot_path{"/run/cns-rpi/cellular_status.json"};
  std::chrono::seconds status_snapshot_max_age{30};
};

/// 视频能力显式启用；工具与暂存目录只由设备配置指定，不由远程动作覆盖。
struct VideoConfig {
  bool enabled{false};
  std::string executable{"/usr/bin/rpicam-vid"};
  std::string ffprobe{"/usr/bin/ffprobe"};
  std::filesystem::path work_directory{"/tmp/cns-rpi-video"};
};

/// RTK 的两个串口独立于 F407；云端身份始终由 serial 链路上的 F407 提供。
struct RtkBaseConfig {
  bool enabled{false};
  std::string device;                     ///< 9Ps 基站端口；启用时必须为明确绝对路径。
  int baud{115200};
  std::string px4_device;                 ///< 3DR/PX4 端口；启用时必须为明确绝对路径。
  int px4_baud{57600};
  std::uint32_t survey_min_duration_s{60};  ///< Survey-in 最短持续时间，10..86400 秒。
  double survey_accuracy_m{2.0};           ///< Survey-in 平均位置精度门限，0.1..100 米。
  std::uint32_t stale_after_ms{3000};      ///< 基站新样本过期门限，1000..60000 毫秒。
  std::filesystem::path status_file{"/run/cns-rpi/rtk-base-status.json"};
};

struct AppConfig {
  SerialConfig serial;
  TelemetryPublishConfig telemetry_publish;
  MqttConfig mqtt;
  LoggingConfig logging;
  IdentityConfig identity;
  device_status::Metadata device_status;
  RuntimeConfig runtime;
  CellularConfig cellular;
  VideoConfig video;
  RtkBaseConfig rtk_base;
};

/**
 * @brief 读取并解析 path 指向的 JSON 配置文件。
 * @return 成功返回填好的 AppConfig；失败返回具体错误原因（不抛异常）。
 */
std::expected<AppConfig, ConfigError> LoadAppConfig(const std::filesystem::path& path);

}  // namespace config
