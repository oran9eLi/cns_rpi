/**
 * @file topic.cpp
 * @brief topic.hpp 的实现。
 */

#include "mqtt/topic.hpp"

namespace mqtt {

namespace {

std::string BuildDeviceTopic(const std::string& topic_namespace, const std::string& device_id,
                             const std::string& suffix) {
  return topic_namespace + "/" + device_id + "/" + suffix;
}

}  // namespace

std::string BuildRegistrationTopic(const std::string& topic_namespace,
                                   const std::string& device_id,
                                   const std::string& suffix) {
  return BuildDeviceTopic(topic_namespace, device_id, suffix);
}

std::string BuildTelemetryTopic(const std::string& topic_namespace, const std::string& device_id,
                                const std::string& suffix) {
  return BuildDeviceTopic(topic_namespace, device_id, suffix);
}

std::string BuildPx4RealtimeTopic(const std::string& topic_namespace,
                                  const std::string& device_id) {
  return BuildDeviceTopic(topic_namespace, device_id, "px4/realtime/v1");
}

std::string BuildPx4LatencyProbeTopic(const std::string& topic_namespace,
                                      const std::string& device_id) {
  return BuildDeviceTopic(topic_namespace, device_id, "px4/latency/probe/v1");
}

std::string BuildPx4LatencyAckTopic(const std::string& topic_namespace,
                                    const std::string& device_id) {
  return BuildDeviceTopic(topic_namespace, device_id, "px4/latency/ack/v1");
}

std::string BuildConfigSetTopic(const std::string& topic_namespace,
                                const std::string& device_id, const std::string& suffix) {
  return BuildDeviceTopic(topic_namespace, device_id, suffix);
}

std::string BuildConfigAckTopic(const std::string& topic_namespace,
                                const std::string& device_id, const std::string& suffix) {
  return BuildDeviceTopic(topic_namespace, device_id, suffix);
}

std::string BuildControlSetTopic(const std::string& topic_namespace,
                                 const std::string& device_id, const std::string& suffix) {
  return BuildDeviceTopic(topic_namespace, device_id, suffix);
}

std::string BuildControlAckTopic(const std::string& topic_namespace,
                                 const std::string& device_id, const std::string& suffix) {
  return BuildDeviceTopic(topic_namespace, device_id, suffix);
}

std::string BuildConfigRequestTopic(const std::string& topic_namespace,
                                    const std::string& source_device_id) {
  return topic_namespace + "/sources/" + source_device_id + "/config/request";
}

}  // namespace mqtt
