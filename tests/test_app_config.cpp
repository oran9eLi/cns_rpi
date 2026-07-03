#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "config/app_config.hpp"

namespace {

std::filesystem::path WriteTempConfig(const std::string& content) {
  auto path = std::filesystem::temp_directory_path() / "cns_rpi_test_config.json";
  std::ofstream out(path, std::ios::trunc);
  out << content;
  return path;
}

}  // namespace

TEST_CASE("完整合法配置文件能正确解析出serial/mqtt/logging字段") {
  auto path = WriteTempConfig(R"({
    "serial": {"device": "/dev/ttyUSB0", "baud": 115200},
    "mqtt": {"broker_host": "192.168.1.100", "broker_port": 1883, "client_id": "cns-rpi",
             "username": "", "password": "", "topic_prefix": "cns_rpi", "qos": 1, "keepalive_seconds": 60},
    "logging": {"level": "info", "file": ""}
  })");

  auto result = config::LoadAppConfig(path);

  REQUIRE(result.has_value());
  CHECK(result->serial.device == "/dev/ttyUSB0");
  CHECK(result->serial.baud == 115200);
  CHECK(result->mqtt.broker_host == "192.168.1.100");
  CHECK(result->mqtt.broker_port == 1883);
  CHECK(result->mqtt.qos == 1);
  CHECK(result->logging.level == "info");
}

TEST_CASE("配置文件不存在时返回kFileNotFound") {
  auto result = config::LoadAppConfig("/nonexistent/path/config.json");

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kFileNotFound);
}

TEST_CASE("JSON格式损坏时返回kParseError") {
  auto path = WriteTempConfig("{ not valid json ");

  auto result = config::LoadAppConfig(path);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kParseError);
}

TEST_CASE("缺少必需字段时返回kMissingField") {
  auto path = WriteTempConfig(R"({"serial": {"device": "/dev/ttyUSB0"}})");

  auto result = config::LoadAppConfig(path);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kMissingField);
}

TEST_CASE("字段存在但类型不对时返回kInvalidValue") {
  auto path = WriteTempConfig(R"({
    "serial": {"device": "/dev/ttyUSB0", "baud": "fast"},
    "mqtt": {"broker_host": "192.168.1.100", "broker_port": 1883, "client_id": "cns-rpi",
             "username": "", "password": "", "topic_prefix": "cns_rpi", "qos": 1, "keepalive_seconds": 60},
    "logging": {"level": "info", "file": ""}
  })");

  auto result = config::LoadAppConfig(path);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kInvalidValue);
}

TEST_CASE("serial有效但mqtt整节缺失时返回kMissingField") {
  auto path = WriteTempConfig(R"({
    "serial": {"device": "/dev/ttyUSB0", "baud": 115200},
    "logging": {"level": "info", "file": ""}
  })");

  auto result = config::LoadAppConfig(path);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kMissingField);
}
