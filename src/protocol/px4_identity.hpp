#pragma once

/**
 * @file px4_identity.hpp
 * @brief PX4 侧的身份/元数据获取：主动请求 MAVLink 消息，以及从
 * AUTOPILOT_VERSION 提取产品与版本元数据。
 *
 * @details
 * PX4 的**身份**来自 OPEN_DRONE_ID_BASIC_ID.uas_id，不来自这里的任何函数——
 * `AUTOPILOT_VERSION.uid/uid2` 标识的是飞控硬件，不是平台管理的受控设备，
 * 已按设计文档 §2.3 从状态和对外 JSON 中彻底移除，不要再加回来。
 * 这个文件只负责两件事：把消息请求打包成 COMMAND_LONG；把 AUTOPILOT_VERSION
 * 收敛成精简的产品/版本结构，好让 state 不必长期持有含 uid/uid2 的完整结构体。
 * 依赖边界：不包含 control_command/、state/ 等模块头文件，目标端点用两个
 * uint8_t 传入，避免 protocol/ 反向依赖命令层。
 */

#include <cstdint>
#include <optional>

#include "common/mavlink.h"
#include "device/product_info.hpp"

namespace protocol {

/**
 * @brief 从 AUTOPILOT_VERSION 提取产品信息，编码按设计文档 §4.3 转十进制字符串。
 * @return `vendor_id` 和 `product_id` 全为 0 时返回 nullopt——PX4 未烧录这两个
 * 值时报 0，输出 "0"/"0" 会让服务器把"未知"当成一个真实型号。
 */
std::optional<device::ProductInfo> ExtractPx4ProductInfo(
    const mavlink_autopilot_version_t& version);

/**
 * @brief 从 AUTOPILOT_VERSION 提取硬件与固件版本。
 * @return 两个字段都拿不到时返回 nullopt；单个字段为 0 视为未知并省略。
 * `flight_sw_version` 按 MAVLink 约定的高 3 字节格式化成 "主.次.修订"。
 */
std::optional<device::VersionInfo> ExtractPx4VersionInfo(
    const mavlink_autopilot_version_t& version);

/**
 * @brief 构造请求任意一条 MAVLink 消息的 MAV_CMD_REQUEST_MESSAGE 命令。
 *
 * 设计文档 §3.2：PX4 在普通遥测链路上既不主动发 OPEN_DRONE_ID_BASIC_ID，
 * 也不主动发 AUTOPILOT_VERSION，两者都要靠这条命令周期请求。
 * COMMAND_ACK 不携带被请求的消息号，调用方需要自己错开两条请求的发送时机，
 * 否则无法从应答判断是哪一条被拒绝。
 */
mavlink_message_t BuildMessageRequest(std::uint32_t message_id,
                                      std::uint8_t source_system_id,
                                      std::uint8_t source_component_id,
                                      std::uint8_t target_system_id,
                                      std::uint8_t target_component_id);

}  // namespace protocol
