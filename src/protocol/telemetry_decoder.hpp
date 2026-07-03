#pragma once

/**
 * @file telemetry_decoder.hpp
 * @brief M3a 范围内 7 种标准 MAVLink 消息的解码入口。
 *
 * @details
 * 只负责"认出 M3a 关心的 7 种消息、调官方 decode 函数、写入 state_store"，
 * 不关心帧是从哪来的（uart/mavlink_link 的事），也不关心解码后数据被谁读、
 * 怎么用（state/、mqtt/ 等下游模块的事）。M3a 之外的消息类型（扩展帧/身份帧/
 * 其他）一律安静忽略，不是错误。
 * 依赖边界：依赖 state/state_store.hpp 和官方 mavlink/c_library_v2 头文件，
 * 不包含 uart/、mqtt/ 等模块头文件。
 */

#include "common/mavlink.h"
#include "state/state_store.hpp"

namespace protocol {

/**
 * @brief 尝试把 msg 解码成 M3a 范围内的 7 种标准消息之一并写入 store。
 * @param msg 已经通过 CRC 校验的完整 MAVLink 帧。
 * @param store 解码结果写入的目标状态存储。
 * @return 是本函数认识的消息类型（成功解码并写入 store）返回 true；
 *         不认识的 msgid 返回 false，不写入、不报错。
 */
bool DecodeAndStore(const mavlink_message_t& msg, state::StateStore& store);

}  // namespace protocol
