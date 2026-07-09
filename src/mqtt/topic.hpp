#pragma once

/**
 * @file topic.hpp
 * @brief MQTT topic 命名规则——纯字符串拼接，不依赖 libmosquitto。
 *
 * @details
 * 拆成独立文件是为了让这部分逻辑本机不装 libmosquitto-dev 也能编译/单测
 * （对比 mqtt_client.hpp/.cpp 必须链接真实的 libmosquitto）。
 * 命名方案见 docs/superpowers/specs/2026-07-09-m5-mqtt-publish-design.md §2。
 * 依赖边界：只依赖标准库，不包含 mosquitto.h/state/ 等其他模块头文件。
 */

#include <string>

namespace mqtt {

/**
 * @brief 拼遥测发布 topic。
 * @param topic_prefix 来自 config.json 的 mqtt.topic_prefix（区分环境/产品线）。
 * @param vendor_id 厂商唯一产品识别码（docs/设备标识符.md 权威全局键，来自
 * state_store.vendor_id，调用方需保证已经有值才调用本函数）。
 * @return "{topic_prefix}/{vendor_id}/telemetry"。不做参数校验（空字符串只是
 * 拼出带空段的字符串，不是这个函数要处理的错误）。
 */
std::string BuildTelemetryTopic(const std::string& topic_prefix, const std::string& vendor_id);

}  // namespace mqtt
