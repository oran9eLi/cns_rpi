#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
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
                     "client_id": "cns-rpi",
                     "reconnect": {"delay_s": 1, "delay_max_s": 30}},
      "auth": {"username": "user", "password": "pass"},
      "topics": {
        "namespace": "cns_rpi",
        "registration": {"suffix": "registration", "qos": 2},
        "telemetry": {"suffix": "telemetry", "qos": 0},
        "config_set": {"suffix": "config/set", "qos": 2},
        "config_ack": {"suffix": "config/ack", "qos": 2}
      }
    },
    "logging": {"level": "info", "file": "", "max_file_size_kb": 1024},
    "identity": {"school_name": "NNUTC"},
    "runtime": {
      "telemetry_publish_interval_ms": 1000,
      "heartbeat_interval_ms": 1000,
      "applied_command_ids": []
    }
  })";
}

std::string ReplaceOnce(std::string text, const std::string& from, const std::string& to) {
  const auto pos = text.find(from);
  REQUIRE(pos != std::string::npos);
  text.replace(pos, from.size(), to);
  return text;
}

std::string ValidConfigWithSerialDevice(const std::string& device) {
  return ReplaceOnce(ValidConfig(), "\"device\": \"/dev/ttyUSB0\"",
                     "\"device\": \"" + device + "\"");
}

}  // namespace

TEST_CASE("auto是合法串口配置值") {
  auto result = config::LoadAppConfig(
      WriteTempConfig(ValidConfigWithSerialDevice("auto")));

  REQUIRE(result.has_value());
  CHECK(result->serial.device == "auto");
}

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
  CHECK(result->mqtt.topics.config_set.suffix == "config/set");
  CHECK(result->mqtt.topics.config_set.qos == 2);
  CHECK(result->mqtt.topics.config_ack.suffix == "config/ack");
  CHECK(result->mqtt.topics.config_ack.qos == 2);
  CHECK(result->logging.level == "info");
  CHECK(result->logging.file.empty());
  CHECK(result->logging.max_file_size_bytes == 1024U * 1024U);
  CHECK(result->runtime.telemetry_publish_interval == std::chrono::milliseconds(1000));
  CHECK(result->runtime.heartbeat_interval == std::chrono::milliseconds(1000));
  CHECK(result->runtime.applied_command_ids.empty());
  CHECK(result->mqtt.connection.reconnect.delay_seconds == 1);
  CHECK(result->mqtt.connection.reconnect.delay_max_seconds == 30);
  CHECK(result->cellular.interface_name == "usb0");
  CHECK(result->cellular.heartbeat_interval == std::chrono::milliseconds(1000));
}

TEST_CASE("日志级别和容量必须合法") {
  for (const auto& replacement : {"\"level\": \"trace\"",
                                  "\"max_file_size_kb\": 63",
                                  "\"max_file_size_kb\": 102401"}) {
    const std::string original = replacement[1] == 'l'
        ? "\"level\": \"info\"" : "\"max_file_size_kb\": 1024";
    auto result = config::LoadAppConfig(
        WriteTempConfig(ReplaceOnce(ValidConfig(), original, replacement)));
    CHECK_FALSE(result.has_value());
  }
}

TEST_CASE("配置命令和ACK必须使用QoS2") {
  auto invalid = ReplaceOnce(ValidConfig(),
      "\"config_set\": {\"suffix\": \"config/set\", \"qos\": 2}",
      "\"config_set\": {\"suffix\": \"config/set\", \"qos\": 1}");
  auto result = config::LoadAppConfig(WriteTempConfig(invalid));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kInvalidValue);
}

TEST_CASE("运行参数范围或重连组合非法时返回kInvalidValue") {
  SUBCASE("遥测间隔低于100毫秒") {
    auto invalid = ReplaceOnce(ValidConfig(), "\"telemetry_publish_interval_ms\": 1000",
                               "\"telemetry_publish_interval_ms\": 99");
    auto result = config::LoadAppConfig(WriteTempConfig(invalid));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
  SUBCASE("心跳间隔高于60000毫秒") {
    auto invalid = ReplaceOnce(ValidConfig(), "\"heartbeat_interval_ms\": 1000",
                               "\"heartbeat_interval_ms\": 60001");
    auto result = config::LoadAppConfig(WriteTempConfig(invalid));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
  SUBCASE("重连初始等待大于最大等待") {
    auto invalid = ReplaceOnce(ValidConfig(), "\"delay_s\": 1, \"delay_max_s\": 30",
                               "\"delay_s\": 31, \"delay_max_s\": 30");
    auto result = config::LoadAppConfig(WriteTempConfig(invalid));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
  SUBCASE("幂等命令号超过32条") {
    std::string ids = "[";
    for (int i = 0; i < 33; ++i) {
      if (i != 0) ids += ",";
      ids += "\"cmd-" + std::to_string(i) + "\"";
    }
    ids += "]";
    auto invalid = ReplaceOnce(ValidConfig(), "\"applied_command_ids\": []",
                               "\"applied_command_ids\": " + ids);
    auto result = config::LoadAppConfig(WriteTempConfig(invalid));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
}

TEST_CASE("配置文件不存在时返回kFileNotFound") {
  auto result = config::LoadAppConfig("/nonexistent/path/config.json");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kFileNotFound);
  CHECK(config::ConfigErrorMessage(result.error()) == "配置文件不存在或无法打开");
}

TEST_CASE("JSON格式损坏时返回kParseError") {
  auto result = config::LoadAppConfig(WriteTempConfig("{ not valid json "));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kParseError);
  CHECK(config::ConfigErrorMessage(result.error()) == "配置文件不是合法JSON");
}

TEST_CASE("缺少必需字段时返回kMissingField") {
  auto result = config::LoadAppConfig(WriteTempConfig(R"({"serial": {"device": "/dev/ttyUSB0"}})"));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kMissingField);
  CHECK(config::ConfigErrorMessage(result.error()) == "配置文件缺少必需字段");
}

TEST_CASE("字段类型错误时返回kInvalidValue") {
  auto invalid = ReplaceOnce(ValidConfig(), "\"baud\": 115200", "\"baud\": \"fast\"");
  auto result = config::LoadAppConfig(WriteTempConfig(invalid));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kInvalidValue);
  CHECK(config::ConfigErrorMessage(result.error()) == "配置字段类型或取值非法");
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
