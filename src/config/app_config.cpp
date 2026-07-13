/**
 * @file app_config.cpp
 * @brief app_config.hpp 的实现。
 */

#include "config/app_config.hpp"

#include <fstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace config {

namespace {

bool IsValidTopicSegment(const std::string& value) {
  return !value.empty() && value.find_first_of("/+#") == std::string::npos;
}

bool IsValidTopicPath(const std::string& value) {
  if (value.empty() || value.front() == '/' || value.back() == '/' ||
      value.find_first_of("+#") != std::string::npos) {
    return false;
  }
  return value.find("//") == std::string::npos;
}

bool IsValidQos(int qos) { return qos >= 0 && qos <= 2; }

bool IsValidCommandId(const std::string& command_id) {
  return !command_id.empty() && command_id.size() <= 128;
}

}  // namespace

std::string_view ConfigErrorMessage(ConfigError error) {
  switch (error) {
    case ConfigError::kFileNotFound:
      return "配置文件不存在或无法打开";
    case ConfigError::kParseError:
      return "配置文件不是合法JSON";
    case ConfigError::kMissingField:
      return "配置文件缺少必需字段";
    case ConfigError::kInvalidValue:
      return "配置字段类型或取值非法";
  }
  return "未知配置错误";
}

std::expected<AppConfig, ConfigError> LoadAppConfig(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    return std::unexpected(ConfigError::kFileNotFound);
  }

  nlohmann::json root;
  try {
    in >> root;
  } catch (const nlohmann::json::parse_error&) {
    return std::unexpected(ConfigError::kParseError);
  }

  AppConfig cfg;
  try {
    const auto& serial = root.at("serial");
    cfg.serial.device = serial.at("device").get<std::string>();
    cfg.serial.baud = serial.at("baud").get<int>();

    const auto& mqtt = root.at("mqtt");
    const auto& connection = mqtt.at("connection");
    cfg.mqtt.connection.host = connection.at("host").get<std::string>();
    cfg.mqtt.connection.port = connection.at("port").get<int>();
    cfg.mqtt.connection.keepalive_seconds = connection.at("keepalive").get<int>();
    cfg.mqtt.connection.client_id_prefix = connection.at("client_id").get<std::string>();
    const auto& reconnect = connection.at("reconnect");
    cfg.mqtt.connection.reconnect.delay_seconds = reconnect.at("delay_s").get<int>();
    cfg.mqtt.connection.reconnect.delay_max_seconds = reconnect.at("delay_max_s").get<int>();

    const auto& auth = mqtt.at("auth");
    cfg.mqtt.auth.username = auth.at("username").get<std::string>();
    cfg.mqtt.auth.password = auth.at("password").get<std::string>();

    const auto& topics = mqtt.at("topics");
    cfg.mqtt.topics.topic_namespace = topics.at("namespace").get<std::string>();
    const auto& registration = topics.at("registration");
    cfg.mqtt.topics.registration.suffix = registration.at("suffix").get<std::string>();
    cfg.mqtt.topics.registration.qos = registration.at("qos").get<int>();
    const auto& telemetry = topics.at("telemetry");
    cfg.mqtt.topics.telemetry.suffix = telemetry.at("suffix").get<std::string>();
    cfg.mqtt.topics.telemetry.qos = telemetry.at("qos").get<int>();
    const auto& config_set = topics.at("config_set");
    cfg.mqtt.topics.config_set.suffix = config_set.at("suffix").get<std::string>();
    cfg.mqtt.topics.config_set.qos = config_set.at("qos").get<int>();
    const auto& config_ack = topics.at("config_ack");
    cfg.mqtt.topics.config_ack.suffix = config_ack.at("suffix").get<std::string>();
    cfg.mqtt.topics.config_ack.qos = config_ack.at("qos").get<int>();

    const auto& logging = root.at("logging");
    cfg.logging.level = logging.at("level").get<std::string>();
    cfg.logging.file = logging.at("file").get<std::string>();

    const auto& identity = root.at("identity");
    cfg.identity.school_name = identity.at("school_name").get<std::string>();

    const auto& runtime = root.at("runtime");
    cfg.runtime.telemetry_publish_interval = std::chrono::milliseconds(
        runtime.at("telemetry_publish_interval_ms").get<int>());
    cfg.runtime.heartbeat_interval =
        std::chrono::milliseconds(runtime.at("heartbeat_interval_ms").get<int>());
    cfg.runtime.applied_command_ids =
        runtime.at("applied_command_ids").get<std::vector<std::string>>();
  } catch (const nlohmann::json::out_of_range&) {
    return std::unexpected(ConfigError::kMissingField);
  } catch (const nlohmann::json::type_error&) {
    return std::unexpected(ConfigError::kInvalidValue);
  }

  const auto& connection = cfg.mqtt.connection;
  const auto& topics = cfg.mqtt.topics;
  const auto telemetry_ms = cfg.runtime.telemetry_publish_interval.count();
  const auto heartbeat_ms = cfg.runtime.heartbeat_interval.count();
  if (connection.host.empty() || connection.client_id_prefix.empty() || connection.port < 1 ||
      connection.port > 65535 || connection.keepalive_seconds <= 0 ||
      connection.reconnect.delay_seconds < 1 || connection.reconnect.delay_seconds > 3600 ||
      connection.reconnect.delay_max_seconds < 1 ||
      connection.reconnect.delay_max_seconds > 3600 ||
      connection.reconnect.delay_seconds > connection.reconnect.delay_max_seconds ||
      telemetry_ms < 100 || telemetry_ms > 60000 || heartbeat_ms < 100 ||
      heartbeat_ms > 60000 || cfg.runtime.applied_command_ids.size() > 32 ||
      !IsValidTopicSegment(topics.topic_namespace) ||
      !IsValidTopicSegment(topics.registration.suffix) ||
      !IsValidTopicSegment(topics.telemetry.suffix) || !IsValidQos(topics.registration.qos) ||
      !IsValidQos(topics.telemetry.qos) || !IsValidTopicPath(topics.config_set.suffix) ||
      !IsValidTopicPath(topics.config_ack.suffix) || topics.config_set.qos != 2 ||
      topics.config_ack.qos != 2) {
    return std::unexpected(ConfigError::kInvalidValue);
  }

  std::unordered_set<std::string> command_ids;
  for (const auto& command_id : cfg.runtime.applied_command_ids) {
    if (!IsValidCommandId(command_id) || !command_ids.insert(command_id).second) {
      return std::unexpected(ConfigError::kInvalidValue);
    }
  }

  return cfg;
}

}  // namespace config
