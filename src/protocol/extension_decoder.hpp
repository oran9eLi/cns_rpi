#pragma once

/**
 * @file extension_decoder.hpp
 * @brief M3b 范围内 NAMED_VALUE_INT/TUNNEL 扩展帧的解码入口。
 *
 * @details
 * 只负责"认出本模块关心的扩展帧语义(M3b: MODSTAT0/MODSTAT1/BAT2STAT/MOTORPWM/
 * GNSS_SAT/ENVHUM 六种 NAMED_VALUE_INT + 告警表/日志增量两种 TUNNEL；
 * M3c: OPEN_DRONE_ID_BASIC_ID/LOCATION/SYSTEM/OPERATOR_ID/SELF_ID 五种身份帧)、
 * 解码、写入 state_store"，不做单位换算(留给 M4 payload/json_serializer)，
 * 不关心帧从哪来(uart/mavlink_link 的事)、被谁读(state/ 下游消费者的事)。
 * M3b 范围外的消息类型/name/payload_type，以及 payload_length 不足以容纳
 * 表头的畸形 TUNNEL 帧，一律安静忽略，不是错误。
 * 依赖边界：依赖 state/state_store.hpp 和官方 mavlink/c_library_v2 头文件，
 * 不包含 uart/、mqtt/ 等模块头文件。跟 telemetry_decoder.hpp 是同一层级的
 * 平行模块，各自处理不同的消息范围，函数名不同(DecodeExtensionAndStore vs
 * DecodeAndStore)以避免 protocol 命名空间下的重复定义。
 */

#include "common/mavlink.h"
#include "state/state_store.hpp"

namespace protocol {

/**
 * @brief 尝试把 msg 解码成 M3b 范围内的扩展帧之一并写入 store。
 * @param msg 已经通过 CRC 校验的完整 MAVLink 帧。
 * @param store 解码结果写入的目标状态存储。
 * @return 是本函数认识的消息类型/name/payload_type(成功解码并写入 store)
 *         返回 true；不认识的一律返回 false，不写入、不报错(含 payload_length
 *         不足以容纳表头的畸形 TUNNEL 帧)。
 */
bool DecodeExtensionAndStore(const mavlink_message_t& msg, state::StateStore& store);

}  // namespace protocol
