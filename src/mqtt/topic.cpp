/**
 * @file topic.cpp
 * @brief topic.hpp 的实现。
 */

#include "mqtt/topic.hpp"

namespace mqtt {

std::string BuildTelemetryTopic(const std::string& topic_prefix, const std::string& vendor_id) {
  return topic_prefix + "/" + vendor_id + "/telemetry";
}

}  // namespace mqtt
