#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "config/startup_summary.hpp"

#include <chrono>

TEST_CASE("启动时用中文展示运行参数和设备接入模式") {
  config::AppConfig value;
  value.telemetry_publish.snapshot = {true, std::chrono::milliseconds(1000)};
  value.telemetry_publish.realtime = {true, std::chrono::milliseconds(100)};
  value.runtime.heartbeat_interval = std::chrono::milliseconds(5000);
  value.device.mode = device::DetectionMode::kAuto;
  value.mqtt.connection.reconnect.delay_seconds = 1;
  value.mqtt.connection.reconnect.delay_max_seconds = 30;
  value.mqtt.topics.topic_namespace = "cns_rpi";
  value.mqtt.topics.runtime_status.suffix = "runtime/status/v1";
  value.identity.binding_file = "/var/lib/cns-rpi/device_binding.json";

  CHECK(config::BuildStartupSummary(value) ==
        "当前运行参数：遥测快照通道=已启用/周期1000毫秒，"
        "遥测实时通道=已启用/周期100毫秒，心跳发送周期=5000毫秒，"
        "设备接入模式=auto，MQTT重连初始等待=1秒，MQTT重连最大等待=30秒，"
        "运行三态Topic=cns_rpi/{device_id}/runtime/status/v1，"
        "设备绑定文件=/var/lib/cns-rpi/device_binding.json");
}
