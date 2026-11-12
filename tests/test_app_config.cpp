#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/app_config.hpp"
#include "config/startup_summary.hpp"

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
  CHECK(result->device_status.device_type == "cns_box");
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
           R"({"device_status":"1"})", R"({"fault_modules":["GNSS"]})",
           R"({"fault_modules":null})", R"({"unknown":"value"})"}) {
    CAPTURE(metadata);
    const auto content = ReplaceOnce(ValidConfig(), "\"serial\":",
        "\"device_status\":" + std::string(metadata) + ",\"serial\":");
    const auto result = config::LoadAppConfig(WriteTempConfig(content));
    CHECK_FALSE(result.has_value());
    if (!result) CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
}

TEST_CASE("GNSS配置在入口拒绝错误类型相对路径和未知字段") {
  for (const auto& gnss : {
           R"(null)", R"([])", R"("not-an-object")",
           R"({"enabled":"true"})", R"({"enabled":1})",
           R"({"enabled":null})", R"({"archive_directory":false})",
           R"({"archive_directory":""})", R"({"archive_directory":"relative"})",
           R"({"enable":true})", R"({"enabled":false,"unexpected":1})"}) {
    const auto content = ReplaceOnce(ValidConfig(), "\"serial\":",
        "\"gnss\":" + std::string(gnss) + ",\"serial\":");
    const auto result = config::LoadAppConfig(WriteTempConfig(content));
    CHECK_FALSE(result.has_value());
    if (!result) CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
}

TEST_CASE("旧配置和空GNSS配置默认关闭采集并保留归档目录") {
  for (const auto& content : {
           ValidConfig(), ReplaceOnce(ValidConfig(), "\"serial\":",
               R"("gnss":{},"serial":)")}) {
    const auto result = config::LoadAppConfig(WriteTempConfig(content));
    REQUIRE(result);
    CHECK_FALSE(result->gnss.enabled);
    CHECK(result->gnss.archive_directory == "/var/lib/cns-rpi/gnss");
  }
}

TEST_CASE("GNSS配置支持显式开关和独立指定归档目录") {
  for (const auto& [gnss, enabled, directory] :
       std::vector<std::tuple<std::string, bool, std::string>>{
           {R"({"enabled":true})", true, "/var/lib/cns-rpi/gnss"},
           {R"({"archive_directory":"/run/cns-rpi/gnss"})", false,
            "/run/cns-rpi/gnss"},
           {R"({"enabled":true,"archive_directory":"/srv/gnss"})", true,
            "/srv/gnss"},
           {R"({"enabled":false,"archive_directory":"/srv/gnss"})", false,
            "/srv/gnss"}}) {
    const auto content = ReplaceOnce(ValidConfig(), "\"serial\":",
        "\"gnss\":" + gnss + ",\"serial\":");
    const auto result = config::LoadAppConfig(WriteTempConfig(content));
    REQUIRE(result);
    CHECK(result->gnss.enabled == enabled);
    CHECK(result->gnss.archive_directory == directory);
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

TEST_CASE("Topic周期配置可重启恢复且缺省不改变原上报行为") {
  const auto original = config::LoadAppConfig(WriteTempConfig(ValidConfig()));
  REQUIRE(original);
  CHECK(original->reporting.runtime_status_interval == std::chrono::milliseconds{10000});
  CHECK(original->reporting.imu_sample_interval == std::chrono::milliseconds{0});
  CHECK(original->reporting.gnss_batch_interval == std::chrono::milliseconds{200});
  auto json = nlohmann::json::parse(ValidConfig());
  json["reporting"] = {{"runtime_status_interval_ms", 2000},
      {"imu_sample_interval_ms", 500}, {"gnss_batch_interval_ms", 1000}};
  json["telemetry_publish"]["realtime"]["interval_ms"] = 60000;
  const auto updated = config::LoadAppConfig(WriteTempConfig(json.dump()));
  REQUIRE(updated);
  CHECK(updated->reporting.runtime_status_interval == std::chrono::milliseconds{2000});
  CHECK(updated->reporting.imu_sample_interval == std::chrono::milliseconds{500});
  CHECK(updated->reporting.gnss_batch_interval == std::chrono::milliseconds{1000});
  CHECK(updated->telemetry_publish.realtime.interval == std::chrono::milliseconds{60000});
  for (const auto& invalid : {nlohmann::json(99), nlohmann::json(60001),
                              nlohmann::json(200.5), nlohmann::json("200")}) {
    json["reporting"]["gnss_batch_interval_ms"] = invalid;
    CHECK_FALSE(config::LoadAppConfig(WriteTempConfig(json.dump())));
  }
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

TEST_CASE("遥测后缀不能占用固定数据和实际命令应答Topic") {
  for (const auto& suffix : {"runtime/status/v1", "imu/sample/v1", "gnss/raw/v1",
                            "registration", "config/set", "config/ack", "control/set",
                            "control/ack", "experiment/set", "experiment/ack", "config/request"}) {
    auto root = nlohmann::json::parse(ValidConfig());
    for (const auto& channel : {"telemetry_snapshot", "telemetry_realtime"}) {
      auto candidate = root;
      candidate["mqtt"]["topics"][channel]["suffix"] = suffix;
      CHECK_FALSE(config::LoadAppConfig(WriteTempConfig(candidate.dump())).has_value());
    }
  }
  auto root = nlohmann::json::parse(ValidConfig());
  root["mqtt"]["topics"]["config_set"]["suffix"] = "maintenance/config";
  root["mqtt"]["topics"]["telemetry_snapshot"]["suffix"] = "maintenance/config";
  CHECK_FALSE(config::LoadAppConfig(WriteTempConfig(root.dump())).has_value());
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
           std::tuple{"实时周期高于60000毫秒",
                      "\"realtime\": {\"enabled\": true, \"interval_ms\": 100}",
                      "\"realtime\": {\"enabled\": true, \"interval_ms\": 60001}"},
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

TEST_CASE("RTK配置拒绝非法类型范围和非绝对状态路径") {
  using Json = nlohmann::json;
  for (const auto& rtk : std::vector<Json>{
           "not-an-object", {{"enabled", "yes"}}, {{"baud", 12345}},
           {{"px4_device", 3}}, {{"px4_baud", "57600"}}, {{"px4_baud", 57600.5}},
           {{"px4_baud", 12345}}, {{"px4_baud", 4295024896ULL}},
           {{"baud", 115200.5}}, {{"survey_min_duration_s", 9}},
           {{"survey_min_duration_s", 86401}}, {{"survey_min_duration_s", 60.5}},
           {{"survey_min_duration_s", 4294967356ULL}},
           {{"survey_accuracy_m", 0.09}}, {{"survey_accuracy_m", 100.1}},
           {{"survey_accuracy_m", "2.0"}}, {{"stale_after_ms", 999}},
           {{"stale_after_ms", 60001}}, {{"status_file", "relative.json"}},
           {{"status_file", ""}}}) {
    auto content = Json::parse(ValidConfig());
    content["rtk_base"] = rtk;
    const auto result = config::LoadAppConfig(WriteTempConfig(content.dump()));
    CHECK_FALSE(result.has_value());
    if (!result) CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
}

TEST_CASE("启用RTK要求基站和PX4明确指定且三个端点互不相同") {
  using Json = nlohmann::json;
  for (const auto& [f407, px4, base] :
       std::vector<std::tuple<std::string, std::string, std::string>>{
           {"auto", "", "/dev/ttyACM0"},
           {"auto", "auto", "/dev/ttyACM0"},
           {"auto", "ttyUSB0", "/dev/ttyACM0"},
           {"auto", "/dev/ttyUSB0", "auto"},
           {"auto", "/dev/ttyUSB0", "ttyACM0"},
           {"auto", "/dev/ttyUSB0", ""},
           {"auto", "/dev/ttyUSB0", "/dev/ttyUSB0"},
           {"auto", "/dev/ttyUSB0", "/dev/../dev/ttyUSB0"},
           {"/dev/ttyUSB2", "/dev/ttyUSB0", "/dev/ttyUSB2"},
           {"/dev/ttyUSB2", "/dev/ttyUSB2", "/dev/ttyACM0"}}) {
    auto content = Json::parse(ValidConfig());
    content["serial"]["device"] = f407;
    content["rtk_base"] = {{"enabled", true}, {"device", base}, {"px4_device", px4}};
    const auto result = config::LoadAppConfig(WriteTempConfig(content.dump()));
    CHECK_FALSE(result.has_value());
    if (!result) CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
}

TEST_CASE("启用RTK拒绝PX4波特率小数与超出整数范围的值") {
  using Json = nlohmann::json;
  for (const auto& baud : std::vector<Json>{115200.5, 4295082496ULL}) {
    auto content = Json::parse(ValidConfig());
    content["rtk_base"] = {{"enabled", true}, {"device", "/dev/ttyACM0"},
        {"px4_device", "/dev/ttyUSB1"}, {"px4_baud", baud}};
    const auto result = config::LoadAppConfig(WriteTempConfig(content.dump()));
    CHECK_FALSE(result.has_value());
    if (!result) CHECK(result.error() == config::ConfigError::kInvalidValue);
  }
}

TEST_CASE("RTK默认关闭且配置中的测量参数不会丢失") {
  using Json = nlohmann::json;
  const auto old = config::LoadAppConfig(WriteTempConfig(ValidConfig()));
  REQUIRE(old);
  CHECK_FALSE(old->rtk_base.enabled);
  CHECK(old->rtk_base.device.empty());
  CHECK(old->rtk_base.baud == 115200);
  CHECK(old->rtk_base.px4_device.empty());
  CHECK(old->rtk_base.px4_baud == 57600);
  CHECK(old->rtk_base.survey_min_duration_s == 60);
  CHECK(old->rtk_base.survey_accuracy_m == 2.0);
  CHECK(old->rtk_base.stale_after_ms == 3000);
  CHECK(old->rtk_base.status_file == "/run/cns-rpi/rtk-base-status.json");
  auto content = Json::parse(ValidConfig());
  content["rtk_base"] = {{"enabled", true}, {"device", "/dev/ttyACM0"},
      {"px4_device", "/dev/ttyUSB1"}, {"px4_baud", 38400},
      {"baud", 230400}, {"survey_min_duration_s", 120},
      {"survey_accuracy_m", 0.5}, {"stale_after_ms", 5000},
      {"status_file", "/run/cns-rpi/base.json"}};
  const auto enabled = config::LoadAppConfig(WriteTempConfig(content.dump()));
  REQUIRE(enabled);
  CHECK(enabled->rtk_base.enabled);
  CHECK(enabled->rtk_base.device == "/dev/ttyACM0");
  CHECK(enabled->rtk_base.baud == 230400);
  CHECK(enabled->rtk_base.px4_device == "/dev/ttyUSB1");
  CHECK(enabled->rtk_base.px4_baud == 38400);
  CHECK(enabled->rtk_base.survey_min_duration_s == 120);
  CHECK(enabled->rtk_base.survey_accuracy_m == 0.5);
  CHECK(enabled->rtk_base.stale_after_ms == 5000);
  CHECK(enabled->rtk_base.status_file == "/run/cns-rpi/base.json");
}

TEST_CASE("RTK配置接受测量边界和所有现有串口波特率") {
  using Json = nlohmann::json;
  for (const auto& [duration, accuracy, stale] :
       std::vector<std::tuple<int, double, int>>{{10, 0.1, 1000}, {86400, 100.0, 60000}}) {
    auto content = Json::parse(ValidConfig());
    content["rtk_base"] = {{"enabled", true}, {"device", "/dev/ttyACM0"},
        {"px4_device", "/dev/ttyUSB1"},
        {"survey_min_duration_s", duration}, {"survey_accuracy_m", accuracy},
        {"stale_after_ms", stale}};
    CHECK(config::LoadAppConfig(WriteTempConfig(content.dump())).has_value());
  }
  for (const int baud : {9600, 19200, 38400, 57600, 115200, 230400}) {
    auto content = Json::parse(ValidConfig());
    content["rtk_base"] = {{"enabled", true}, {"device", "/dev/ttyACM0"},
        {"px4_device", "/dev/ttyUSB1"}, {"baud", baud}, {"px4_baud", baud}};
    CHECK(config::LoadAppConfig(WriteTempConfig(content.dump())).has_value());
  }
}

TEST_CASE("RTK启用后启动摘要保留F407主控箱并标注RTK能力") {
  config::AppConfig value;
  value.rtk_base.enabled = true;
  const auto summary = config::BuildStartupSummary(value);
  CHECK(summary.find("受控设备=F407主控箱") != std::string::npos);
  CHECK(summary.find("RTK基站实验=已启用") != std::string::npos);
}

TEST_CASE("三个串口不能通过符号链接占用同一个端点") {
  using Json = nlohmann::json;
  const auto directory = std::filesystem::temp_directory_path() / "cns_rpi_rtk_serial_alias_test";
  std::filesystem::create_directories(directory);
  const auto device = directory / "device";
  const auto alias = directory / "by-id";
  std::ofstream(device).close();
  std::filesystem::create_symlink(device, alias);
  auto content = Json::parse(ValidConfig());
  std::vector<bool> accepted;
  for (const auto& [f407, px4, base] :
       std::vector<std::tuple<std::string, std::string, std::string>>{
           {device.string(), "/dev/ttyUSB1", alias.string()},
           {device.string(), alias.string(), "/dev/ttyACM0"},
           {"auto", device.string(), alias.string()}}) {
    content["serial"]["device"] = f407;
    content["rtk_base"] = {{"enabled", true}, {"device", base}, {"px4_device", px4}};
    accepted.push_back(config::LoadAppConfig(WriteTempConfig(content.dump())).has_value());
  }
  std::filesystem::remove(alias);
  std::filesystem::remove(device);
  std::filesystem::remove(directory);
  for (const bool result : accepted) CHECK_FALSE(result);
}

TEST_CASE("RTK启用时F407可自动发现且其波特率配置保持原行为") {
  using Json = nlohmann::json;
  for (const auto& [f407, baud] :
       std::vector<std::pair<std::string, int>>{{"auto", 115200}, {"/dev/ttyUSB2", 12345}}) {
    auto content = Json::parse(ValidConfig());
    content["serial"] = {{"device", f407}, {"baud", baud}};
    content["rtk_base"] = {{"enabled", true}, {"device", "/dev/ttyACM0"},
        {"px4_device", "/dev/ttyUSB0"}};
    const auto result = config::LoadAppConfig(WriteTempConfig(content.dump()));
    REQUIRE(result);
    CHECK(result->serial.device == f407);
    CHECK(result->serial.baud == baud);
  }
}
