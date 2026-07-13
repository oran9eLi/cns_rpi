/**
 * @file config_request.cpp
 * @brief config_request.hpp 的实现。
 */

#include "config_command/config_request.hpp"

#include <cctype>

#include "mqtt/mqtt_client.hpp"
#include "mqtt/topic.hpp"

namespace config_command {
namespace {
bool IsValidDcdwLabel(std::string_view label) {
  return label.size() == 8 && label.starts_with("DCDW-") &&
         std::isdigit(static_cast<unsigned char>(label[5])) &&
         std::isdigit(static_cast<unsigned char>(label[6])) &&
         std::isdigit(static_cast<unsigned char>(label[7]));
}

nlohmann::json ParametersToJson(const ConfigParameterPatch& parameters) {
  nlohmann::json json = nlohmann::json::object();
  if (parameters.telemetry_publish_interval_ms) {
    json["telemetry_publish_interval_ms"] = *parameters.telemetry_publish_interval_ms;
  }
  if (parameters.heartbeat_interval_ms) {
    json["heartbeat_interval_ms"] = *parameters.heartbeat_interval_ms;
  }
  if (parameters.mqtt_reconnect_delay_s) {
    json["mqtt_reconnect_delay_s"] = *parameters.mqtt_reconnect_delay_s;
  }
  if (parameters.mqtt_reconnect_delay_max_s) {
    json["mqtt_reconnect_delay_max_s"] = *parameters.mqtt_reconnect_delay_max_s;
  }
  return json;
}
}  // namespace

std::expected<nlohmann::json, CommandError> BuildConfigRequestPayload(
    std::string_view request_id, std::string_view target_dcdw_label,
    const ConfigParameterPatch& parameters) {
  if (request_id.empty() || request_id.size() > 128) {
    return std::unexpected(
        CommandError{.code = "invalid_request_id", .message = "request_id长度非法"});
  }
  if (!IsValidDcdwLabel(target_dcdw_label)) {
    return std::unexpected(
        CommandError{.code = "invalid_target", .message = "目标内部编号格式非法"});
  }
  auto parameter_json = ParametersToJson(parameters);
  if (parameter_json.empty()) {
    return std::unexpected(
        CommandError{.code = "invalid_parameter", .message = "参数补丁不能为空"});
  }
  return nlohmann::json{{"request_id", request_id},
                        {"target", {{"dcdw_label", target_dcdw_label}}},
                        {"parameters", std::move(parameter_json)}};
}

bool PublishConfigRequest(mqtt::MqttClient& client,
                          const std::string& topic_namespace,
                          const std::string& source_vendor_id,
                          const std::string& request_id,
                          const std::string& target_dcdw_label,
                          const ConfigParameterPatch& parameters) {
  auto payload = BuildConfigRequestPayload(request_id, target_dcdw_label, parameters);
  if (!payload) return false;
  return client.Publish(mqtt::BuildConfigRequestTopic(topic_namespace, source_vendor_id),
                        payload->dump(), /*qos=*/2, /*retain=*/false);
}

}  // namespace config_command
