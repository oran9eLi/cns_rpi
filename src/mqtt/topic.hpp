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
 * @param device_id 受控设备主键，来自 OPEN_DRONE_ID_BASIC_ID.uas_id
 * （主控箱和 PX4 同一来源，见 docs/2026-08-03-主控箱与PX4统一身份数据结构设计.md
 * §2.1；调用方需保证已经有值才调用本函数）。
 * @param suffix 来自 mqtt.topics.registration.suffix。
 * @return "{namespace}/{device_id}/{suffix}"，不重复做配置校验。
 */
std::string BuildRegistrationTopic(const std::string& topic_namespace,
                                   const std::string& device_id,
                                   const std::string& suffix);

/// 拼遥测发布 topic，参数语义同 BuildRegistrationTopic，suffix 来自 telemetry 配置。
std::string BuildTelemetryTopic(const std::string& topic_namespace, const std::string& device_id,
                                const std::string& suffix);

/// 服务端向 PX4 树莓派发送真实 RTT 探测的 QoS 0 topic。
std::string BuildPx4LatencyProbeTopic(const std::string& topic_namespace,
                                      const std::string& device_id);

/// PX4 树莓派原样返回 RTT 探测关联标识符的 QoS 0 topic。
std::string BuildPx4LatencyAckTopic(const std::string& topic_namespace,
                                    const std::string& device_id);

/// 拼服务器向设备下发配置命令的 topic。
std::string BuildConfigSetTopic(const std::string& topic_namespace,
                                const std::string& device_id, const std::string& suffix);

/// 拼设备返回配置命令执行结果的 topic。
std::string BuildConfigAckTopic(const std::string& topic_namespace,
                                const std::string& device_id, const std::string& suffix);

/// 拼服务器向设备下发飞行控制命令的 topic。
std::string BuildControlSetTopic(const std::string& topic_namespace,
                                 const std::string& device_id, const std::string& suffix);

/// 拼设备返回单片机真实执行结果的 topic。
std::string BuildControlAckTopic(const std::string& topic_namespace,
                                 const std::string& device_id, const std::string& suffix);

/// 拼本设备作为命令来源时向服务器提交请求的 topic。
std::string BuildConfigRequestTopic(const std::string& topic_namespace,
                                    const std::string& source_device_id);

}  // namespace mqtt
