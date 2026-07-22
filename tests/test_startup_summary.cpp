#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "config/startup_summary.hpp"

#include <chrono>
#include <string>

TEST_CASE("启动配置摘要覆盖生效参数且不泄露MQTT凭据") {
  config::AppConfig value;
  value.serial = {.device = "auto", .baud = 115200};
  value.mqtt.connection = {.host = "112.124.52.232",
                           .port = 1883,
                           .keepalive_seconds = 60,
                           .client_id_prefix = "cns-rpi",
                           .reconnect = {.delay_seconds = 2,
                                         .delay_max_seconds = 30}};
  value.mqtt.auth = {.username = "secret-user", .password = "secret-password"};
  value.mqtt.topics = {
      .topic_namespace = "cns",
      .registration = {.suffix = "registration", .qos = 1},
      .telemetry = {.suffix = "telemetry", .qos = 1},
      .config_set = {.suffix = "config/set", .qos = 2},
      .config_ack = {.suffix = "config/ack", .qos = 2},
      .control_set = {.suffix = "control/set", .qos = 2},
      .control_ack = {.suffix = "control/ack", .qos = 2},
  };
  value.logging = {.level = "debug",
                   .file = "/var/log/cns-rpi.log",
                   .max_file_size_bytes = 1024 * 1024};
  value.identity.school_name = "示例学校";
  value.runtime.telemetry_publish_interval = std::chrono::milliseconds(1000);
  value.runtime.heartbeat_interval = std::chrono::milliseconds(2000);
  value.runtime.applied_command_ids = {"request-1", "request-2"};
  value.cellular = {.interface_name = "usb0",
                    .heartbeat_interval = std::chrono::milliseconds(3000),
                    .status_snapshot_path = "/run/cns-rpi/cellular_status.json",
                    .status_snapshot_max_age = std::chrono::seconds(30)};

  const std::string summary =
      config::BuildStartupSummary(value, "/etc/cns-rpi/config.json");

  CHECK(summary.find("当前应用配置:") != std::string::npos);
  CHECK(summary.find("config=/etc/cns-rpi/config.json") != std::string::npos);
  CHECK(summary.find("serial.device=auto") != std::string::npos);
  CHECK(summary.find("serial.baud=115200") != std::string::npos);
  CHECK(summary.find("mqtt.host=112.124.52.232") != std::string::npos);
  CHECK(summary.find("mqtt.port=1883") != std::string::npos);
  CHECK(summary.find("mqtt.keepalive_s=60") != std::string::npos);
  CHECK(summary.find("mqtt.client_id_prefix=cns-rpi") != std::string::npos);
  CHECK(summary.find("mqtt.reconnect_delay_s=2") != std::string::npos);
  CHECK(summary.find("mqtt.reconnect_delay_max_s=30") != std::string::npos);
  CHECK(summary.find("mqtt.username=已配置") != std::string::npos);
  CHECK(summary.find("mqtt.password=已配置") != std::string::npos);
  CHECK(summary.find("topics.namespace=cns") != std::string::npos);
  CHECK(summary.find("topics.registration=registration(qos=1)") != std::string::npos);
  CHECK(summary.find("topics.telemetry=telemetry(qos=1)") != std::string::npos);
  CHECK(summary.find("topics.config_set=config/set(qos=2)") != std::string::npos);
  CHECK(summary.find("topics.config_ack=config/ack(qos=2)") != std::string::npos);
  CHECK(summary.find("topics.control_set=control/set(qos=2)") != std::string::npos);
  CHECK(summary.find("topics.control_ack=control/ack(qos=2)") != std::string::npos);
  CHECK(summary.find("logging.level=debug") != std::string::npos);
  CHECK(summary.find("logging.output=/var/log/cns-rpi.log") != std::string::npos);
  CHECK(summary.find("logging.max_file_size_bytes=1048576") != std::string::npos);
  CHECK(summary.find("school_name=示例学校") != std::string::npos);
  CHECK(summary.find("telemetry_publish_interval_ms=1000") != std::string::npos);
  CHECK(summary.find("heartbeat_interval_ms=2000") != std::string::npos);
  CHECK(summary.find("applied_command_ids=2") != std::string::npos);
  CHECK(summary.find("cellular.interface_name=usb0") != std::string::npos);
  CHECK(summary.find("cellular.heartbeat_interval_ms=3000") != std::string::npos);
  CHECK(summary.find("cellular.status_snapshot_path=/run/cns-rpi/cellular_status.json") !=
        std::string::npos);
  CHECK(summary.find("cellular.status_snapshot_max_age_s=30") != std::string::npos);
  CHECK(summary.find("secret-user") == std::string::npos);
  CHECK(summary.find("secret-password") == std::string::npos);
  CHECK(summary.find('\n') == std::string::npos);
}

TEST_CASE("空MQTT凭据和标准输出在摘要中明确标识") {
  config::AppConfig value;
  value.logging.file.clear();

  const std::string summary = config::BuildStartupSummary(value, "/config.json");

  CHECK(summary.find("mqtt.username=未配置") != std::string::npos);
  CHECK(summary.find("mqtt.password=未配置") != std::string::npos);
  CHECK(summary.find("logging.output=stdout/stderr") != std::string::npos);
}
