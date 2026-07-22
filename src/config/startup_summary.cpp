#include "config/startup_summary.hpp"

#include <sstream>
#include <string_view>

namespace config {
namespace {

std::string_view Configured(const std::string& value) {
  return value.empty() ? "未配置" : "已配置";
}

void AppendTopic(std::ostringstream& out, std::string_view name,
                 const MqttTopicConfig& topic) {
  out << ' ' << name << '=' << topic.suffix << "(qos=" << topic.qos << ')';
}

}  // namespace

std::string BuildStartupSummary(const AppConfig& config,
                                const std::filesystem::path& config_path) {
  const auto& connection = config.mqtt.connection;
  const auto& topics = config.mqtt.topics;
  std::ostringstream out;
  out << "当前应用配置: config=" << config_path.string()
      << " serial.device=" << config.serial.device
      << " serial.baud=" << config.serial.baud
      << " mqtt.host=" << connection.host
      << " mqtt.port=" << connection.port
      << " mqtt.keepalive_s=" << connection.keepalive_seconds
      << " mqtt.client_id_prefix=" << connection.client_id_prefix
      << " mqtt.reconnect_delay_s=" << connection.reconnect.delay_seconds
      << " mqtt.reconnect_delay_max_s=" << connection.reconnect.delay_max_seconds
      << " mqtt.username=" << Configured(config.mqtt.auth.username)
      << " mqtt.password=" << Configured(config.mqtt.auth.password)
      << " topics.namespace=" << topics.topic_namespace;
  AppendTopic(out, "topics.registration", topics.registration);
  AppendTopic(out, "topics.telemetry", topics.telemetry);
  AppendTopic(out, "topics.config_set", topics.config_set);
  AppendTopic(out, "topics.config_ack", topics.config_ack);
  AppendTopic(out, "topics.control_set", topics.control_set);
  AppendTopic(out, "topics.control_ack", topics.control_ack);
  out << " logging.level=" << config.logging.level
      << " logging.output="
      << (config.logging.file.empty() ? "stdout/stderr" : config.logging.file)
      << " logging.max_file_size_bytes=" << config.logging.max_file_size_bytes
      << " school_name=" << config.identity.school_name
      << " telemetry_publish_interval_ms="
      << config.runtime.telemetry_publish_interval.count()
      << " heartbeat_interval_ms=" << config.runtime.heartbeat_interval.count()
      << " applied_command_ids=" << config.runtime.applied_command_ids.size()
      << " cellular.interface_name=" << config.cellular.interface_name
      << " cellular.heartbeat_interval_ms=" << config.cellular.heartbeat_interval.count()
      << " cellular.status_snapshot_path=" << config.cellular.status_snapshot_path.string()
      << " cellular.status_snapshot_max_age_s="
      << config.cellular.status_snapshot_max_age.count();
  return out.str();
}

}  // namespace config
