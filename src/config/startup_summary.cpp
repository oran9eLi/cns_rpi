#include "config/startup_summary.hpp"

#include <sstream>

namespace config {

std::string BuildStartupSummary(const AppConfig& config) {
  std::ostringstream out;
  out << "当前运行参数：遥测上报周期="
      << config.runtime.telemetry_publish_interval.count()
      << "毫秒，心跳发送周期=" << config.runtime.heartbeat_interval.count()
      << "毫秒，MQTT重连初始等待="
      << config.mqtt.connection.reconnect.delay_seconds
      << "秒，MQTT重连最大等待="
      << config.mqtt.connection.reconnect.delay_max_seconds << "秒";
  return out.str();
}

}  // namespace config
