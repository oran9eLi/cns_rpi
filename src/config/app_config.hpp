#pragma once

/**
 * @file app_config.hpp
 * @brief 读取 config/config.json，给各模块提供启动配置。
 *
 * @details
 * M2 阶段只有 uart/ 消费 serial 字段；mqtt/logging 字段现在就解析出来放进
 * AppConfig（对应 config.example.json 的完整 schema），但暂时没人用——
 * 等 M5（MQTT）、日志接入这些模块实现时再从这里取，不需要再改这个文件的 schema。
 * 依赖边界：只依赖 nlohmann/json，不包含 uart/、mqtt/ 等其他模块头文件。
 */

#include <expected>
#include <filesystem>
#include <string>

namespace config {

/// 配置加载失败的原因。
enum class ConfigError {
  kFileNotFound,   ///< 文件路径打不开
  kParseError,     ///< 内容不是合法 JSON
  kMissingField,   ///< 缺少必需字段
  kInvalidValue,   ///< 字段存在但类型不对
};

struct SerialConfig {
  std::string device;  ///< 字符设备路径，例如 "/dev/ttyUSB0"
  int baud = 0;        ///< 波特率
};

struct MqttConfig {
  std::string broker_host;
  int broker_port = 0;
  std::string client_id;
  std::string username;
  std::string password;
  std::string topic_prefix;
  int qos = 0;
  int keepalive_seconds = 0;
};

struct LoggingConfig {
  std::string level;
  std::string file;
};

struct AppConfig {
  SerialConfig serial;
  MqttConfig mqtt;
  LoggingConfig logging;
};

/**
 * @brief 读取并解析 path 指向的 JSON 配置文件。
 * @return 成功返回填好的 AppConfig；失败返回具体错误原因（不抛异常）。
 */
std::expected<AppConfig, ConfigError> LoadAppConfig(const std::filesystem::path& path);

}  // namespace config
