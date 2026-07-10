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

std::string ValidConfig() {
  return R"({
    "serial": {"device": "/dev/ttyUSB0", "baud": 115200},
    "mqtt": {
      "connection": {"host": "localhost", "port": 1883, "keepalive": 60,
                     "client_id": "cns-rpi"},
      "auth": {"username": "user", "password": "pass"},
      "topics": {
        "namespace": "cns_rpi",
        "registration": {"suffix": "registration", "qos": 2},
        "telemetry": {"suffix": "telemetry", "qos": 0}
      }
    },
    "logging": {"level": "info", "file": ""},
    "identity": {"school_name": "NNUTC"}
  })";
}

std::string ReplaceOnce(std::string text, const std::string& from, const std::string& to) {
  const auto pos = text.find(from);
  REQUIRE(pos != std::string::npos);
  text.replace(pos, from.size(), to);
  return text;
}

}  // namespace

TEST_CASE("完整合法嵌套配置能正确解析") {
  auto result = config::LoadAppConfig(WriteTempConfig(ValidConfig()));

  REQUIRE(result.has_value());
  CHECK(result->serial.device == "/dev/ttyUSB0");
  CHECK(result->mqtt.connection.host == "localhost");
  CHECK(result->mqtt.connection.port == 1883);
  CHECK(result->mqtt.connection.keepalive_seconds == 60);
  CHECK(result->mqtt.connection.client_id_prefix == "cns-rpi");
  CHECK(result->mqtt.auth.username == "user");
  CHECK(result->mqtt.topics.topic_namespace == "cns_rpi");
  CHECK(result->mqtt.topics.registration.suffix == "registration");
  CHECK(result->mqtt.topics.registration.qos == 2);
  CHECK(result->mqtt.topics.telemetry.suffix == "telemetry");
  CHECK(result->mqtt.topics.telemetry.qos == 0);
}

TEST_CASE("配置文件不存在时返回kFileNotFound") {
  auto result = config::LoadAppConfig("/nonexistent/path/config.json");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kFileNotFound);
}

TEST_CASE("JSON格式损坏时返回kParseError") {
  auto result = config::LoadAppConfig(WriteTempConfig("{ not valid json "));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kParseError);
}

TEST_CASE("缺少必需字段时返回kMissingField") {
  auto result = config::LoadAppConfig(WriteTempConfig(R"({"serial": {"device": "/dev/ttyUSB0"}})"));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kMissingField);
}

TEST_CASE("字段类型错误时返回kInvalidValue") {
  auto invalid = ReplaceOnce(ValidConfig(), "\"baud\": 115200", "\"baud\": \"fast\"");
  auto result = config::LoadAppConfig(WriteTempConfig(invalid));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kInvalidValue);
}

TEST_CASE("MQTT数值范围非法时返回kInvalidValue") {
  SUBCASE("端口为0") {
    auto result = config::LoadAppConfig(
        WriteTempConfig(ReplaceOnce(ValidConfig(), "\"port\": 1883", "\"port\": 0")));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
  SUBCASE("keepalive为0") {
    auto result = config::LoadAppConfig(
        WriteTempConfig(ReplaceOnce(ValidConfig(), "\"keepalive\": 60", "\"keepalive\": 0")));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
  SUBCASE("registration QoS为3") {
    auto result = config::LoadAppConfig(
        WriteTempConfig(ReplaceOnce(ValidConfig(), "\"qos\": 2", "\"qos\": 3")));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
  SUBCASE("telemetry QoS为负数") {
    auto result = config::LoadAppConfig(
        WriteTempConfig(ReplaceOnce(ValidConfig(), "\"qos\": 0", "\"qos\": -1")));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
}

TEST_CASE("MQTT topic段非法时返回kInvalidValue") {
  SUBCASE("namespace为空") {
    auto result = config::LoadAppConfig(WriteTempConfig(
        ReplaceOnce(ValidConfig(), "\"namespace\": \"cns_rpi\"", "\"namespace\": \"\"")));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
  SUBCASE("suffix含斜杠") {
    auto result = config::LoadAppConfig(WriteTempConfig(ReplaceOnce(
        ValidConfig(), "\"suffix\": \"registration\"", "\"suffix\": \"bad/topic\"")));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
  SUBCASE("suffix含单层通配符") {
    auto result = config::LoadAppConfig(WriteTempConfig(
        ReplaceOnce(ValidConfig(), "\"suffix\": \"telemetry\"", "\"suffix\": \"+\"")));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
  SUBCASE("namespace含多层通配符") {
    auto result = config::LoadAppConfig(WriteTempConfig(
        ReplaceOnce(ValidConfig(), "\"namespace\": \"cns_rpi\"", "\"namespace\": \"#\"")));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
}
