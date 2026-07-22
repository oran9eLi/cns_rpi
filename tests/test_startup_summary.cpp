#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "config/startup_summary.hpp"

#include <chrono>

TEST_CASE("启动时只用中文展示四个可配置运行参数") {
  config::AppConfig value;
  value.runtime.telemetry_publish_interval = std::chrono::milliseconds(1000);
  value.runtime.heartbeat_interval = std::chrono::milliseconds(5000);
  value.mqtt.connection.reconnect.delay_seconds = 1;
  value.mqtt.connection.reconnect.delay_max_seconds = 30;

  CHECK(config::BuildStartupSummary(value) ==
        "当前运行参数：遥测上报周期=1000毫秒，心跳发送周期=5000毫秒，"
        "MQTT重连初始等待=1秒，MQTT重连最大等待=30秒");
}
