#pragma once

/**
 * @file app_config.hpp
 * @brief 读取 config/config.json，给各模块提供启动配置。
 *
 * @details
 * MQTT 配置按 connection/auth/topics 分组，配置加载阶段完成类型、范围和
 * topic 单段合法性校验，运行循环只消费已经验证的值。
 * 依赖边界：只依赖 nlohmann/json，不包含 uart/、mqtt/ 等其他模块头文件。
 */

#include <expected>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "device/device_type.hpp"

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

struct DeviceConfig {
  device::DetectionMode mode{device::DetectionMode::kAuto};
};

/// PX4 专用 QGC 自动发现配置；可同时允许局域网和 WireGuard 安全隧道入口。
struct QgcUdpConfig {
  bool enabled{false};
  std::vector<std::string> lan_interfaces{"wlan0", "eth0"};
  std::string peer_policy{"first_valid_wins"};
  int listen_port{14540};
  int qgc_port{14550};
  std::chrono::milliseconds discovery_interval{1000};
  std::chrono::milliseconds handover_idle{2500};
  std::chrono::milliseconds peer_timeout{5000};
  bool allow_commands{true};
};

/// PX4 Web 控制台专用高频遥测；只对真实飞控生效，主控箱继续使用原遥测周期。
struct Px4RealtimeConfig {
  bool enabled{true};
  std::chrono::milliseconds publish_interval{50};
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
  MqttTopicConfig telemetry;
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
};

struct RuntimeConfig {
  std::chrono::milliseconds telemetry_publish_interval{0};
  std::chrono::milliseconds heartbeat_interval{0};
  std::vector<std::string> applied_command_ids;
};

struct CellularConfig {
  std::string interface_name{"usb0"};
  std::chrono::milliseconds heartbeat_interval{1000};
  std::filesystem::path status_snapshot_path{"/run/cns-rpi/cellular_status.json"};
  std::chrono::seconds status_snapshot_max_age{30};
};

struct AppConfig {
  SerialConfig serial;
  DeviceConfig device;
  QgcUdpConfig qgc_udp;
  Px4RealtimeConfig px4_realtime;
  MqttConfig mqtt;
  LoggingConfig logging;
  IdentityConfig identity;
  RuntimeConfig runtime;
  CellularConfig cellular;
};

/**
 * @brief 读取并解析 path 指向的 JSON 配置文件。
 * @return 成功返回填好的 AppConfig；失败返回具体错误原因（不抛异常）。
 */
std::expected<AppConfig, ConfigError> LoadAppConfig(const std::filesystem::path& path);

}  // namespace config
