/**
 * @file app_config.cpp
 * @brief app_config.hpp 的实现。
 */

#include "config/app_config.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

namespace config {

namespace {

bool IsValidTopicSegment(const std::string& value) {
  return !value.empty() && value.find_first_of("/+#") == std::string::npos;
}

bool IsValidQos(int qos) { return qos >= 0 && qos <= 2; }

}  // namespace

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

    const auto& logging = root.at("logging");
    cfg.logging.level = logging.at("level").get<std::string>();
    cfg.logging.file = logging.at("file").get<std::string>();

    const auto& identity = root.at("identity");
    cfg.identity.school_name = identity.at("school_name").get<std::string>();
  } catch (const nlohmann::json::out_of_range&) {
    return std::unexpected(ConfigError::kMissingField);
  } catch (const nlohmann::json::type_error&) {
    return std::unexpected(ConfigError::kInvalidValue);
  }

  const auto& connection = cfg.mqtt.connection;
  const auto& topics = cfg.mqtt.topics;
  if (connection.host.empty() || connection.client_id_prefix.empty() || connection.port < 1 ||
      connection.port > 65535 || connection.keepalive_seconds <= 0 ||
      !IsValidTopicSegment(topics.topic_namespace) ||
      !IsValidTopicSegment(topics.registration.suffix) ||
      !IsValidTopicSegment(topics.telemetry.suffix) || !IsValidQos(topics.registration.qos) ||
      !IsValidQos(topics.telemetry.qos)) {
    return std::unexpected(ConfigError::kInvalidValue);
  }

  return cfg;
}

}  // namespace config
