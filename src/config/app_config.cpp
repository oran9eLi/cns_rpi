/**
 * @file app_config.cpp
 * @brief app_config.hpp 的实现。
 */

#include "config/app_config.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

namespace config {

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
    cfg.mqtt.broker_host = mqtt.at("broker_host").get<std::string>();
    cfg.mqtt.broker_port = mqtt.at("broker_port").get<int>();
    cfg.mqtt.client_id = mqtt.at("client_id").get<std::string>();
    cfg.mqtt.username = mqtt.at("username").get<std::string>();
    cfg.mqtt.password = mqtt.at("password").get<std::string>();
    cfg.mqtt.topic_prefix = mqtt.at("topic_prefix").get<std::string>();
    cfg.mqtt.qos = mqtt.at("qos").get<int>();
    cfg.mqtt.keepalive_seconds = mqtt.at("keepalive_seconds").get<int>();

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

  return cfg;
}

}  // namespace config
