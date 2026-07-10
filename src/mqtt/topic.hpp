#pragma once

/**
 * @file topic.hpp
 * @brief MQTT topic 命名规则——纯字符串拼接，不依赖 libmosquitto。
 *
 * @details
 * 拆成独立文件是为了让这部分逻辑本机不装 libmosquitto-dev 也能编译/单测
 * （对比 mqtt_client.hpp/.cpp 必须链接真实的 libmosquitto）。
 * 命名方案见 docs/superpowers/specs/2026-07-10-mqtt-registration-discovery-design.md。
 * 依赖边界：只依赖标准库，不包含 mosquitto.h/state/ 等其他模块头文件。
 */

#include <string>

namespace mqtt {

/**
 * @brief 拼设备注册 topic。
 * @param topic_namespace 来自 mqtt.topics.namespace。
 * @param vendor_id 厂商唯一产品识别码（docs/设备标识符.md 权威全局键，来自
 * state_store.vendor_id，调用方需保证已经有值才调用本函数）。
 * @param suffix 来自 mqtt.topics.registration.suffix。
 * @return "{namespace}/{vendor_id}/{suffix}"，不重复做配置校验。
 */
std::string BuildRegistrationTopic(const std::string& topic_namespace,
                                   const std::string& vendor_id,
                                   const std::string& suffix);

/// 拼遥测发布 topic，参数语义同 BuildRegistrationTopic，suffix 来自 telemetry 配置。
std::string BuildTelemetryTopic(const std::string& topic_namespace, const std::string& vendor_id,
                                const std::string& suffix);

}  // namespace mqtt
