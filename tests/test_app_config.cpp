#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

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
        "telemetry_snapshot": {"suffix": "telemetry/snapshot/v1", "qos": 0},
        "telemetry_realtime": {"suffix": "telemetry/realtime/v1", "qos": 0},
        "runtime_status": {"suffix": "runtime/status/v1", "qos": 1},
        "config_set": {"suffix": "config/set", "qos": 2},
        "config_ack": {"suffix": "config/ack", "qos": 2}
      }
    },
    "logging": {"level": "info", "file": "", "max_file_size_kb": 1024},
    "identity": {"school_name": "NNUTC"},
    "telemetry_publish": {
      "snapshot": {"enabled": true, "interval_ms": 1000},
      "realtime": {"enabled": true, "interval_ms": 100}
    },
    "runtime": {
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

TEST_CASE("常规资料缺省沿用已确定名称类型与能力列表") {
  const auto result = config::LoadAppConfig(WriteTempConfig(ValidConfig()));
  REQUIRE(result);
  CHECK(result->device_status.device_name == "CNS 一代实训平台");
  CHECK(result->device_status.device_type == "xn_box");
  CHECK(result->device_status.capabilities == std::vector<std::string>{
      "telemetry", "remote_id", "runtime_config", "box_private_control"});
  CHECK_FALSE(result->device_status.dcdw_label.has_value());
  CHECK_FALSE(result->device_status.product.has_value());
}

TEST_CASE("本地常规资料覆盖值和中文学校名称从真实配置加载") {
  auto content = ReplaceOnce(ValidConfig(), "\"serial\":",
      R"("device_status":{"device_name":"教学平台甲","device_type":"custom_box","dcdw_label":"实验箱甲","capabilities":["telemetry","lab"],"product":{"manufacturer_code":"TEST","model_code":"CNS2"}},"serial":)");
  content = ReplaceOnce(content, "NNUTC", "示例职业技术学院");
  const auto result = config::LoadAppConfig(WriteTempConfig(content));
  REQUIRE(result);
  CHECK(result->identity.school_name == "示例职业技术学院");
  CHECK(result->device_status.device_name == "教学平台甲");
  CHECK(result->device_status.device_type == "custom_box");
  REQUIRE(result->device_status.dcdw_label);
  CHECK(*result->device_status.dcdw_label == "实验箱甲");
  CHECK(result->device_status.capabilities == std::vector<std::string>{"telemetry", "lab"});
  REQUIRE(result->device_status.product);
  CHECK(result->device_status.product->manufacturer_code == "TEST");
  CHECK(result->device_status.product->model_code == "CNS2");
}

TEST_CASE("显式空能力列表保留为空而不回填默认值") {
  const auto content = ReplaceOnce(ValidConfig(), "\"serial\":",
      R"("device_status":{"capabilities":[]},"serial":)");
  const auto result = config::LoadAppConfig(WriteTempConfig(content));
  REQUIRE(result);
  CHECK(result->device_status.capabilities.empty());
  CHECK(result->device_status.device_name == "CNS 一代实训平台");
}

TEST_CASE("常规资料入口拒绝非法形状空字符串与只读字段") {
  for (const auto& metadata : {
           R"(null)", R"([])", R"("metadata")",
           R"({"device_name":""})", R"({"device_name":1})",
           R"({"device_type":""})", R"({"dcdw_label":null})",
           R"({"dcdw_label":""})", R"({"capabilities":"telemetry"})",
           R"({"capabilities":[1]})", R"({"capabilities":[""]})",
           R"({"product":null})", R"({"product":{}})",
           R"({"product":{"manufacturer_code":"DCDW"}})",
           R"({"product":{"manufacturer_code":"","model_code":"CNS1"}})",
           R"({"product":{"manufacturer_code":"DCDW","model_code":2}})",
           R"({"device_id":"other"})", R"({"model_version":"9.9.9"})",
           R"({"online":"1"})", R"({"school_id":"另一学校"})",
           R"({"device_status":"1"})", R"({"unknown":"value"})"}) {
    CAPTURE(metadata);
    const auto content = ReplaceOnce(ValidConfig(), "\"serial\":",
        "\"device_status\":" + std::string(metadata) + ",\"serial\":");
    const auto result = config::LoadAppConfig(WriteTempConfig(content));
    CHECK_FALSE(result.has_value());
    if (!result) CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
}

TEST_CASE("视频配置在入口拒绝错误类型与相对工具路径") {
  for (const auto& video : {
           R"({"enabled":"yes"})", R"({"executable":"rpicam-vid"})",
           R"({"ffprobe":"ffprobe"})", R"({"work_directory":"relative"})",
           R"("not-an-object")"}) {
    const auto content = ReplaceOnce(ValidConfig(), "\"serial\":",
        "\"video\":" + std::string(video) + ",\"serial\":");
    const auto result = config::LoadAppConfig(WriteTempConfig(content));
    CHECK_FALSE(result.has_value());
    if (!result) CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
}

TEST_CASE("旧配置不启用视频且显式视频工具配置会保留") {
  const auto old = config::LoadAppConfig(WriteTempConfig(ValidConfig()));
  REQUIRE(old);
  CHECK_FALSE(old->video.enabled);
  CHECK(old->video.executable == "/usr/bin/rpicam-vid");
  const auto content = ReplaceOnce(ValidConfig(), "\"serial\":",
      R"("video":{"enabled":true,"executable":"/opt/rpicam-vid","ffprobe":"/opt/ffprobe","work_directory":"/run/cns-rpi/video"},"serial":)");
  const auto enabled = config::LoadAppConfig(WriteTempConfig(content));
  REQUIRE(enabled);
  CHECK(enabled->video.enabled);
  CHECK(enabled->video.executable == "/opt/rpicam-vid");
  CHECK(enabled->video.ffprobe == "/opt/ffprobe");
  CHECK(enabled->video.work_directory == "/run/cns-rpi/video");
}

TEST_CASE("auto是合法串口配置值") {
  auto result = config::LoadAppConfig(
      WriteTempConfig(ValidConfigWithSerialDevice("auto")));

  REQUIRE(result.has_value());
  CHECK(result->serial.device == "auto");
}

TEST_CASE("旧主控箱设备模式配置可在加载入口兼容") {
  for (const auto& mode : {"auto", "cns_box"}) {
    const auto configured = ReplaceOnce(
        ValidConfig(), "\"serial\": {\"device\": \"/dev/ttyUSB0\", \"baud\": 115200},",
        "\"serial\": {\"device\": \"/dev/ttyUSB0\", \"baud\": 115200},"
        "\n    \"device\": {\"mode\": \"" +
            std::string{mode} + "\"},");
    REQUIRE(config::LoadAppConfig(WriteTempConfig(configured)).has_value());
  }
}

TEST_CASE("旧PX4模式和未知设备模式必须明确拒绝") {
  for (const auto& mode : {"px4", "other"}) {
    const auto configured = ReplaceOnce(
        ValidConfig(), "\"serial\": {\"device\": \"/dev/ttyUSB0\", \"baud\": 115200},",
        "\"serial\": {\"device\": \"/dev/ttyUSB0\", \"baud\": 115200},"
        "\n    \"device\": {\"mode\": \"" +
            std::string{mode} + "\"},");
    const auto result = config::LoadAppConfig(WriteTempConfig(configured));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
}

TEST_CASE("旧QGC UDP配置只兼容显式关闭状态") {
  const auto configured = ReplaceOnce(
      ValidConfig(), "\"serial\": {\"device\": \"/dev/ttyUSB0\", \"baud\": 115200},",
      "\"serial\": {\"device\": \"/dev/ttyUSB0\", \"baud\": 115200},\n"
      "    \"qgc_udp\": {\"enabled\": false},");
  REQUIRE(config::LoadAppConfig(WriteTempConfig(configured)).has_value());
}

TEST_CASE("旧QGC UDP启用配置必须明确拒绝") {
  const auto configured = ReplaceOnce(
      ValidConfig(), "\"serial\": {\"device\": \"/dev/ttyUSB0\", \"baud\": 115200},",
      "\"serial\": {\"device\": \"/dev/ttyUSB0\", \"baud\": 115200},\n"
      "    \"qgc_udp\": {\"enabled\": true},");
  const auto result = config::LoadAppConfig(WriteTempConfig(configured));
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == config::ConfigError::kInvalidValue);
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
  CHECK(result->mqtt.topics.telemetry_snapshot.suffix ==
        "telemetry/snapshot/v1");
  CHECK(result->mqtt.topics.telemetry_snapshot.qos == 0);
  CHECK(result->mqtt.topics.telemetry_realtime.suffix ==
        "telemetry/realtime/v1");
  CHECK(result->mqtt.topics.telemetry_realtime.qos == 0);
  CHECK(result->mqtt.topics.runtime_status.suffix == "runtime/status/v1");
  CHECK(result->mqtt.topics.runtime_status.qos == 1);
  CHECK(result->mqtt.topics.config_set.suffix == "config/set");
  CHECK(result->mqtt.topics.config_set.qos == 2);
  CHECK(result->mqtt.topics.config_ack.suffix == "config/ack");
  CHECK(result->mqtt.topics.config_ack.qos == 2);
  CHECK(result->logging.level == "info");
  CHECK(result->logging.file.empty());
  CHECK(result->logging.max_file_size_bytes == 1024U * 1024U);
  CHECK(result->telemetry_publish.snapshot.enabled);
  CHECK(result->telemetry_publish.snapshot.interval ==
        std::chrono::milliseconds(1000));
  CHECK(result->telemetry_publish.realtime.enabled);
  CHECK(result->telemetry_publish.realtime.interval ==
        std::chrono::milliseconds(100));
  CHECK(result->runtime.heartbeat_interval == std::chrono::milliseconds(1000));
  CHECK(result->runtime.applied_command_ids.empty());
  CHECK(result->identity.binding_file ==
        "/var/lib/cns-rpi/device_binding.json");
  CHECK(result->mqtt.connection.reconnect.delay_seconds == 1);
  CHECK(result->mqtt.connection.reconnect.delay_max_seconds == 30);
  CHECK(result->cellular.interface_name == "usb0");
  CHECK(result->cellular.heartbeat_interval == std::chrono::milliseconds(1000));
  CHECK(result->cellular.status_snapshot_path == "/run/cns-rpi/cellular_status.json");
  CHECK(result->cellular.status_snapshot_max_age == std::chrono::seconds(30));
}

TEST_CASE("身份绑定文件路径可配置且必须为绝对路径") {
  const auto configured = ReplaceOnce(
      ValidConfig(), "\"identity\": {\"school_name\": \"NNUTC\"}",
      "\"identity\": {\"school_name\": \"NNUTC\", "
      "\"binding_file\": \"/tmp/cns-binding.json\"}");
  const auto result = config::LoadAppConfig(WriteTempConfig(configured));
  REQUIRE(result.has_value());
  CHECK(result->identity.binding_file == "/tmp/cns-binding.json");

  const auto relative = ReplaceOnce(
      configured, "\"binding_file\": \"/tmp/cns-binding.json\"",
      "\"binding_file\": \"binding.json\"");
  CHECK_FALSE(config::LoadAppConfig(WriteTempConfig(relative)).has_value());
}

TEST_CASE("运行三态Topic配置必须与冻结契约一致") {
  for (const auto& replacement : {
           "\"runtime_status\": {\"suffix\": \"other/status\", \"qos\": 1}",
           "\"runtime_status\": {\"suffix\": \"runtime/status/v1\", \"qos\": 0}",
       }) {
    const auto invalid = ReplaceOnce(
        ValidConfig(),
        "\"runtime_status\": {\"suffix\": \"runtime/status/v1\", \"qos\": 1}",
        replacement);
    CHECK_FALSE(config::LoadAppConfig(WriteTempConfig(invalid)).has_value());
  }
}

TEST_CASE("5G状态快照配置可覆盖默认值") {
  const auto config = ReplaceOnce(
      ValidConfig(),
      "\"applied_command_ids\": []\n    }",
      "\"applied_command_ids\": []\n    },\n"
      "    \"cellular\": {\"status_snapshot_path\": \"/run/custom/status.json\", "
      "\"status_snapshot_max_age_seconds\": 45}");

  const auto result = config::LoadAppConfig(WriteTempConfig(config));

  REQUIRE(result.has_value());
  CHECK(result->cellular.status_snapshot_path == "/run/custom/status.json");
  CHECK(result->cellular.status_snapshot_max_age == std::chrono::seconds(45));
}

TEST_CASE("5G状态快照路径和过期阈值必须合法") {
  for (const auto& cellular : {
           "{\"status_snapshot_path\": \"\", \"status_snapshot_max_age_seconds\": 30}",
           "{\"status_snapshot_path\": \"relative.json\", "
           "\"status_snapshot_max_age_seconds\": 30}",
           "{\"status_snapshot_path\": \"/run/status.json\", "
           "\"status_snapshot_max_age_seconds\": 9}",
           "{\"status_snapshot_path\": \"/run/status.json\", "
           "\"status_snapshot_max_age_seconds\": 301}",
       }) {
    const auto config = ReplaceOnce(
        ValidConfig(),
        "\"applied_command_ids\": []\n    }",
        "\"applied_command_ids\": []\n    },\n    \"cellular\": " + std::string(cellular));
    const auto result = config::LoadAppConfig(WriteTempConfig(config));
    CHECK_FALSE(result.has_value());
  }
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
  for (const auto& [name, original, replacement] : {
           std::tuple{"快照周期低于100毫秒", "\"interval_ms\": 1000",
                      "\"interval_ms\": 99"},
           std::tuple{"快照周期高于60000毫秒", "\"interval_ms\": 1000",
                      "\"interval_ms\": 60001"},
           std::tuple{"实时周期低于50毫秒",
                      "\"realtime\": {\"enabled\": true, \"interval_ms\": 100}",
                      "\"realtime\": {\"enabled\": true, \"interval_ms\": 49}"},
           std::tuple{"实时周期高于1000毫秒",
                      "\"realtime\": {\"enabled\": true, \"interval_ms\": 100}",
                      "\"realtime\": {\"enabled\": true, \"interval_ms\": 1001}"},
       }) {
    SUBCASE(name) {
      auto result = config::LoadAppConfig(
          WriteTempConfig(ReplaceOnce(ValidConfig(), original, replacement)));
      REQUIRE_FALSE(result.has_value());
      CHECK(result.error() == config::ConfigError::kInvalidValue);
    }
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
  SUBCASE("快照QoS不为0") {
    auto result = config::LoadAppConfig(
        WriteTempConfig(ReplaceOnce(ValidConfig(), "\"qos\": 0", "\"qos\": 1")));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
  SUBCASE("实时QoS不为0") {
    auto invalid = ReplaceOnce(
        ValidConfig(),
        "\"telemetry_realtime\": {\"suffix\": \"telemetry/realtime/v1\", \"qos\": 0}",
        "\"telemetry_realtime\": {\"suffix\": \"telemetry/realtime/v1\", \"qos\": 1}");
    auto result = config::LoadAppConfig(WriteTempConfig(invalid));
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
  SUBCASE("快照suffix为空") {
    auto result = config::LoadAppConfig(WriteTempConfig(
        ReplaceOnce(ValidConfig(), "\"suffix\": \"telemetry/snapshot/v1\"",
                    "\"suffix\": \"\"")));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
  SUBCASE("两个遥测suffix不能相同") {
    auto result = config::LoadAppConfig(WriteTempConfig(ReplaceOnce(
        ValidConfig(), "\"suffix\": \"telemetry/realtime/v1\"",
        "\"suffix\": \"telemetry/snapshot/v1\"")));
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
