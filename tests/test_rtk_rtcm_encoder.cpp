#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <vector>

#include "rtk/rtcm_encoder.hpp"

TEST_CASE("RTCM official decode preserves bytes and hand-derived boundaries") {
  struct Case {
    std::size_t size;
    std::vector<unsigned> lengths, flags;
    unsigned next;
  };
  for (const auto &c : std::vector<Case>{
           {1, {1}, {248}, 0},
           {179, {179}, {248}, 0},
           {180, {180}, {248}, 0},
           {181, {180, 1}, {249, 251}, 0},
           {360, {180, 180, 0}, {249, 251, 253}, 0},
           {540, {180, 180, 180, 0}, {249, 251, 253, 255}, 0},
           {720, {180, 180, 180, 180}, {249, 251, 253, 255}, 0},
           {721, {180, 180, 180, 180, 1}, {249, 251, 253, 255, 0}, 1},
           {1029,
            {180, 180, 180, 180, 180, 129},
            {249, 251, 253, 255, 1, 3},
            1}}) {
    CAPTURE(c.size);
    std::vector<std::uint8_t> input(c.size);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = i % 251;
    std::uint8_t sequence = 31;
    const auto messages = rtk::EncodeRtcm(input, sequence);
    REQUIRE(messages.size() == c.lengths.size());
    CHECK(sequence == c.next);
    std::vector<std::uint8_t> restored;
    for (std::size_t i = 0; i < messages.size(); ++i) {
      const auto &message = messages[i];
      CHECK(message.msgid == 233);
      CHECK(message.sysid == 255);
      CHECK(message.compid == MAV_COMP_ID_MISSIONPLANNER);
      mavlink_gps_rtcm_data_t packet{};
      mavlink_msg_gps_rtcm_data_decode(&message, &packet);
      CHECK(packet.len == c.lengths[i]);
      CHECK(packet.flags == c.flags[i]);
      restored.insert(restored.end(), packet.data, packet.data + packet.len);
    }
    CHECK(restored == input);
  }
}
TEST_CASE("empty input emits nothing and does not consume sequence") {
  std::uint8_t sequence = 4;
  CHECK(rtk::EncodeRtcm({}, sequence).empty());
  CHECK(sequence == 4);
}
