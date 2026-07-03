#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "common/mavlink.h"
#include "uart/mavlink_link.hpp"

namespace {

/// 用官方 pack/to_send_buffer 现造一条合法的 HEARTBEAT 帧字节序列，测试不手写帧格式。
std::vector<std::uint8_t> PackHeartbeatBytes() {
  mavlink_message_t msg{};
  mavlink_msg_heartbeat_pack(/*system_id=*/1, /*component_id=*/MAV_COMP_ID_ONBOARD_COMPUTER, &msg,
                              MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID,
                              /*base_mode=*/0, /*custom_mode=*/0, MAV_STATE_ACTIVE);
  std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> buffer{};
  std::uint16_t len = mavlink_msg_to_send_buffer(buffer.data(), &msg);
  return std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + len);
}

}  // namespace

TEST_CASE("完整合法帧一次性喂入能被正确解出") {
  uart::MavlinkFrameAssembler assembler;
  auto bytes = PackHeartbeatBytes();

  auto result = assembler.Feed(bytes);

  REQUIRE(result.has_value());
  // msgid 是 mavlink_message_t 里的位域(uint32_t:24)，doctest 的 CHECK 展开需要绑定
  // 一个普通引用，位域绑不了引用——GCC 15(开发机)没触发，GCC 14(RPi 真机)会编译报错，
  // static_cast 成普通 uint32_t 值就没有这个问题。
  CHECK(static_cast<std::uint32_t>(result->msgid) == MAVLINK_MSG_ID_HEARTBEAT);

  mavlink_heartbeat_t decoded{};
  mavlink_msg_heartbeat_decode(&*result, &decoded);
  CHECK(decoded.type == MAV_TYPE_ONBOARD_CONTROLLER);
  CHECK(decoded.autopilot == MAV_AUTOPILOT_INVALID);
}

TEST_CASE("CRC被篡改的帧不会被当成合法帧返回") {
  uart::MavlinkFrameAssembler assembler;
  auto bytes = PackHeartbeatBytes();
  // MAVLink v2 头部固定10字节，第10个字节(索引10)是payload第一字节；
  // 篡改它能破坏CRC又不改变帧长度字段，帧结构仍完整、只是CRC校验会失败。
  bytes[10] ^= 0xFF;

  auto result = assembler.Feed(bytes);

  CHECK_FALSE(result.has_value());
}

TEST_CASE("一帧被拆成两次读取仍能拼出完整帧") {
  uart::MavlinkFrameAssembler assembler;
  auto bytes = PackHeartbeatBytes();
  std::size_t split = bytes.size() / 2;
  std::vector<std::uint8_t> first_half(bytes.begin(), bytes.begin() + split);
  std::vector<std::uint8_t> second_half(bytes.begin() + split, bytes.end());

  auto first_result = assembler.Feed(first_half);
  CHECK_FALSE(first_result.has_value());

  auto second_result = assembler.Feed(second_half);
  REQUIRE(second_result.has_value());
  CHECK(static_cast<std::uint32_t>(second_result->msgid) == MAVLINK_MSG_ID_HEARTBEAT);
}

TEST_CASE("合法帧前混入垃圾字节不影响帧被正确解出") {
  uart::MavlinkFrameAssembler assembler;
  std::vector<std::uint8_t> bytes = {0x00, 0xFF, 0x12, 0x34};
  auto heartbeat = PackHeartbeatBytes();
  bytes.insert(bytes.end(), heartbeat.begin(), heartbeat.end());

  auto result = assembler.Feed(bytes);

  REQUIRE(result.has_value());
  CHECK(static_cast<std::uint32_t>(result->msgid) == MAVLINK_MSG_ID_HEARTBEAT);
}
