#include "config/startup_summary.hpp"

#include <sstream>

namespace config {

std::string BuildStartupSummary(const AppConfig& config) {
  std::ostringstream out;
  out << "当前运行参数：遥测快照通道="
      << (config.telemetry_publish.snapshot.enabled ? "已启用" : "已禁用")
      << "/周期" << config.telemetry_publish.snapshot.interval.count()
      << "毫秒，遥测实时通道="
      << (config.telemetry_publish.realtime.enabled ? "已启用" : "已禁用")
      << "/周期" << config.telemetry_publish.realtime.interval.count()
      << "毫秒，心跳发送周期=" << config.runtime.heartbeat_interval.count()
      << "毫秒，设备接入模式="
      << device::DetectionModeName(config.device.mode)
      << "，MQTT重连初始等待="
      << config.mqtt.connection.reconnect.delay_seconds
      << "秒，MQTT重连最大等待="
      << config.mqtt.connection.reconnect.delay_max_seconds << "秒";
  return out.str();
}

}  // namespace config
