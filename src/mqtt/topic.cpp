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

}  // namespace mqtt
