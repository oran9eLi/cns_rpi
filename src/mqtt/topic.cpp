/**
 * @file topic.cpp
 * @brief topic.hpp 的实现。
 */

#include "mqtt/topic.hpp"

namespace mqtt {

namespace {

std::string BuildDeviceTopic(const std::string& topic_namespace, const std::string& vendor_id,
                             const std::string& suffix) {
  return topic_namespace + "/" + vendor_id + "/" + suffix;
}

}  // namespace

std::string BuildRegistrationTopic(const std::string& topic_namespace,
                                   const std::string& vendor_id,
                                   const std::string& suffix) {
  return BuildDeviceTopic(topic_namespace, vendor_id, suffix);
}

std::string BuildTelemetryTopic(const std::string& topic_namespace, const std::string& vendor_id,
                                const std::string& suffix) {
  return BuildDeviceTopic(topic_namespace, vendor_id, suffix);
}

std::string BuildConfigSetTopic(const std::string& topic_namespace,
                                const std::string& vendor_id, const std::string& suffix) {
  return BuildDeviceTopic(topic_namespace, vendor_id, suffix);
}

std::string BuildConfigAckTopic(const std::string& topic_namespace,
                                const std::string& vendor_id, const std::string& suffix) {
  return BuildDeviceTopic(topic_namespace, vendor_id, suffix);
}

std::string BuildControlSetTopic(const std::string& topic_namespace,
                                 const std::string& vendor_id, const std::string& suffix) {
  return BuildDeviceTopic(topic_namespace, vendor_id, suffix);
}

std::string BuildControlAckTopic(const std::string& topic_namespace,
                                 const std::string& vendor_id, const std::string& suffix) {
  return BuildDeviceTopic(topic_namespace, vendor_id, suffix);
}

std::string BuildConfigRequestTopic(const std::string& topic_namespace,
                                    const std::string& source_vendor_id) {
  return topic_namespace + "/sources/" + source_vendor_id + "/config/request";
}

}  // namespace mqtt
