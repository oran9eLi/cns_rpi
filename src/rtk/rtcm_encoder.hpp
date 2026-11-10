/** @file rtcm_encoder.hpp
 * @brief 将完整改正帧有序编码给下挂 PX4，不代表接收机确认。
 */
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "common/mavlink.h"
namespace rtk {
/** @brief 每组最多720字节消耗一个5位序号；调用者保证整帧发送不交错。 */
std::vector<mavlink_message_t> EncodeRtcm(std::span<const std::uint8_t> bytes,
                                          std::uint8_t &sequence);
}  // namespace rtk
