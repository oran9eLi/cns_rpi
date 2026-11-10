#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include "gnss/raw_frame.hpp"

namespace {

using namespace std::chrono_literals;
const gnss::Assembler::Clock::time_point kNow{};
using Payload = std::array<std::uint8_t, 128>;

void Put16(Payload& payload, std::size_t offset, std::uint16_t value) {
  payload[offset] = static_cast<std::uint8_t>(value);
  payload[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void Put32(Payload& payload, std::size_t offset, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    payload[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

mavlink_message_t Pack(const Payload& payload, std::uint8_t length,
                       std::uint8_t system = 42, std::uint8_t component = 193,
                       std::uint16_t type = 0x8020, std::uint8_t target = 0) {
  mavlink_message_t packed{};
  mavlink_msg_tunnel_pack(system, component, &packed, target, 0, type, length,
                          payload.data());
  std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> bytes{};
  const auto size = mavlink_msg_to_send_buffer(bytes.data(), &packed);
  mavlink_message_t parsed{};
  mavlink_message_t buffer{};
  mavlink_status_t status{};
  mavlink_status_t output_status{};
  bool received = false;
  for (std::size_t i = 0; i < size; ++i) {
    received |= mavlink_frame_char_buffer(&buffer, &status, bytes[i], &parsed,
                                         &output_status) == MAVLINK_FRAMING_OK;
  }
  REQUIRE(received);
  return parsed;
}

std::vector<std::uint8_t> Sequence(std::size_t length) {
  std::vector<std::uint8_t> bytes(length);
  for (std::size_t i = 0; i < length; ++i) {
    bytes[i] = static_cast<std::uint8_t>(i);
  }
  return bytes;
}

std::vector<Payload> Fragments(const std::vector<std::uint8_t>& wire,
                               std::uint16_t crc, std::uint8_t protocol = 1,
                               std::uint32_t frame_id = 7) {
  std::vector<Payload> fragments;
  const auto count = (wire.size() + 39) / 40;
  for (std::size_t index = 0; index < count; ++index) {
    Payload payload{};
    payload[0] = 1;
    payload[1] = protocol;
    payload[2] = 3;
    payload[3] = 24;
    Put32(payload, 4, frame_id);
    Put32(payload, 8, 0x12345678);
    Put16(payload, 12, static_cast<std::uint16_t>(wire.size()));
    Put16(payload, 14, static_cast<std::uint16_t>(40 * index));
    payload[16] = static_cast<std::uint8_t>(index);
    payload[17] = static_cast<std::uint8_t>(count);
    payload[18] = static_cast<std::uint8_t>(std::min<std::size_t>(40, wire.size() - index * 40));
    payload[19] = protocol == 1 ? 1 : 0;
    payload[20] = protocol == 2 ? 1 : 0;
    payload[21] = protocol == 2 ? 7 : 0;
    Put16(payload, 22, crc);
    std::copy_n(wire.begin() + 40 * index, payload[18], payload.begin() + 24);
    fragments.push_back(payload);
  }
  return fragments;
}

mavlink_message_t Message(const Payload& payload) {
  return Pack(payload, static_cast<std::uint8_t>(24 + payload[18]));
}

}  // namespace

TEST_CASE("single NMEA restores bytes and envelope without parsing GPS") {
  gnss::Assembler assembler;
  const std::vector<std::uint8_t> wire{'1','2','3','4','5','6','7','8','9'};
  const Payload payload{1,1,3,24,0,0,0,0,0,0,0,0,9,0,0,0,
                        0,1,9,1,0,0,0xB1,0x29,'1','2','3','4','5','6','7','8','9'};
  const auto frame = assembler.Add(Message(payload), kNow);
  REQUIRE(frame.has_value());
  CHECK(frame->wire == wire);
  CHECK(frame->system_id == 42);
  CHECK(frame->component_id == 193);
  CHECK(frame->protocol == 1);
  CHECK(frame->flags == 3);
  CHECK(frame->nmea_type == 1);
  CHECK(frame->ubx_class == 0);
  CHECK(frame->ubx_id == 0);
  CHECK(frame->frame_id == 0);
  CHECK(frame->received_uptime_ms == 0);
  CHECK(frame->wire_crc16 == 0x29B1);
  CHECK(assembler.Stats().frames_completed == 1);
}

TEST_CASE("source checksum BAD preserves NMEA CRLF and still completes") {
  gnss::Assembler assembler;
  const std::vector<std::uint8_t> wire{36,71,80,71,71,65,44,49,42,48,48,13,10};
  auto payload = Fragments(wire, 0xD34A).front();
  payload[2] = 2;
  const auto frame = assembler.Add(Message(payload), kNow);
  REQUIRE(frame.has_value());
  CHECK(frame->wire == wire);
  CHECK(frame->flags == 2);
}

TEST_CASE("40 byte boundaries and NMEA maximum restore exactly out of order") {
  for (const auto [length, crc] : std::array<std::pair<std::size_t,std::uint16_t>, 5>{{
           {40,0x0476}, {41,0x93EE}, {80,0x4EAA}, {81,0x59FF}, {128,0x1800}}}) {
    CAPTURE(length);
    gnss::Assembler assembler;
    const auto wire = Sequence(length);
    auto fragments = Fragments(wire, crc);
    std::reverse(fragments.begin(), fragments.end());
    for (std::size_t i = 0; i < fragments.size(); ++i) {
      const auto frame = assembler.Add(Message(fragments[i]), kNow);
      if (i + 1 == fragments.size()) {
        REQUIRE(frame.has_value());
        CHECK(frame->wire == wire);
      } else {
        CHECK_FALSE(frame.has_value());
      }
    }
    CHECK(assembler.Stats().frames_completed == 1);
  }
}

TEST_CASE("UBX maximum reassembles all 26 fragments with duplicates") {
  gnss::Assembler assembler;
  const auto wire = Sequence(1024);
  const auto fragments = Fragments(wire, 0x758F, 2);
  CHECK_FALSE(assembler.Add(Message(fragments[25]), kNow));
  CHECK_FALSE(assembler.Add(Message(fragments[25]), kNow + 1ms));
  for (std::size_t i = 0; i < 25; ++i) {
    const auto frame = assembler.Add(Message(fragments[i]), kNow + 2ms);
    if (i == 24) {
      REQUIRE(frame);
      CHECK(frame->wire == wire);
      CHECK(frame->ubx_class == 1);
      CHECK(frame->ubx_id == 7);
    } else {
      CHECK_FALSE(frame);
    }
  }
  CHECK(assembler.Stats().fragments_received == 27);
  CHECK(assembler.Stats().duplicate_fragments == 1);
}

TEST_CASE("MAVLink2 truncated trailing zeros are restored by standard decode") {
  gnss::Assembler assembler;
  const std::vector<std::uint8_t> wire{0xB5,0x62,1,7,0,0,0,0};
  const auto message = Message(Fragments(wire, 0x3350, 2).front());
  CHECK(message.len < 5 + 24 + wire.size());
  const auto frame = assembler.Add(message, kNow);
  REQUIRE(frame);
  CHECK(frame->wire == wire);
}

TEST_CASE("unrelated traffic and invalid raw sources are ignored") {
  gnss::Assembler assembler;
  const auto payload = Fragments(Sequence(1), 0xE1F0).front();
  auto message = Message(payload);
  message.msgid = MAVLINK_MSG_ID_HEARTBEAT;
  CHECK_FALSE(assembler.Add(message, kNow));
  CHECK_FALSE(assembler.Add(Pack(payload, 25, 42, 193, 0x8003), kNow));
  for (const auto system : {0,251,255}) {
    CHECK_FALSE(assembler.Add(Pack(payload, 25, system), kNow));
  }
  CHECK_FALSE(assembler.Add(Pack(payload, 25, 42, 192), kNow));
  CHECK(assembler.Stats().fragments_received == 0);
  CHECK(assembler.Add(Pack(payload, 25, 250), kNow).has_value());
}

TEST_CASE("malformed header metadata and noncanonical fragment ranges are rejected") {
  const auto good = Fragments(Sequence(81), 0x59FF).front();
  const std::vector<std::function<void(Payload&)>> corruptions{
      [](auto& p){p[0]=2;}, [](auto& p){p[1]=0;}, [](auto& p){p[1]=3;},
      [](auto& p){p[2]=0;}, [](auto& p){p[2]=1;}, [](auto& p){p[2]=0xFF;},
      [](auto& p){p[3]=23;}, [](auto& p){Put16(p,12,0);},
      [](auto& p){Put16(p,12,129);}, [](auto& p){Put16(p,14,1);},
      [](auto& p){p[16]=3;}, [](auto& p){p[17]=0;}, [](auto& p){p[17]=4;},
      [](auto& p){p[18]=0;}, [](auto& p){p[18]=39;}, [](auto& p){p[18]=41;},
      [](auto& p){p[19]=5;}, [](auto& p){p[20]=1;}, [](auto& p){p[21]=1;},
      [](auto& p){p[1]=2;},
      [](auto& p){p[1]=2;p[19]=0;Put16(p,12,1025);p[17]=26;}};
  for (std::size_t i = 0; i < corruptions.size(); ++i) {
    CAPTURE(i);
    gnss::Assembler assembler;
    auto bad = good;
    corruptions[i](bad);
    CHECK_FALSE(assembler.Add(Message(bad), kNow));
    CHECK(assembler.Stats().invalid_fragments == 1);
    CHECK(assembler.Stats().frames_completed == 0);
  }
  for (const auto length : {0,23,24,63,65,129}) {
    gnss::Assembler assembler;
    CHECK_FALSE(assembler.Add(Pack(good, length), kNow));
    CHECK(assembler.Stats().invalid_fragments == 1);
  }
  gnss::Assembler assembler;
  CHECK_FALSE(assembler.Add(Pack(good, 64, 42, 193, 0x8020, 42), kNow));
  CHECK(assembler.Stats().invalid_fragments == 1);
  auto last = Fragments(Sequence(81), 0x59FF).back();
  last[18] = 2;
  CHECK_FALSE(assembler.Add(Message(last), kNow));
}

TEST_CASE("metadata and same-index byte conflicts discard the whole key for five seconds") {
  const auto fragments = Fragments(Sequence(80), 0x4EAA);
  const std::vector<std::function<void(Payload&)>> conflicts{
      [](auto& p){p[2]=2;}, [](auto& p){p[19]=2;},
      [](auto& p){Put16(p,22,0);}, [](auto& p){Put16(p,12,79);p[18]=39;},
      [](auto& p){p[1]=2;p[19]=0;p[20]=1;p[21]=7;}};
  for (const auto& mutate : conflicts) {
    gnss::Assembler assembler;
    CHECK_FALSE(assembler.Add(Message(fragments[0]), kNow));
    auto changed = fragments[1];
    mutate(changed);
    CHECK_FALSE(assembler.Add(Message(changed), kNow + 1ms));
    CHECK_FALSE(assembler.Add(Message(fragments[0]), kNow + 2ms));
    CHECK_FALSE(assembler.Add(Message(fragments[1]), kNow + 2ms));
    CHECK(assembler.Stats().conflicting_frames == 1);
    CHECK(assembler.Stats().frames_completed == 0);
    CHECK_FALSE(assembler.Add(Message(fragments[0]), kNow + 5001ms));
    CHECK(assembler.Add(Message(fragments[1]), kNow + 5001ms).has_value());
  }
  gnss::Assembler assembler;
  CHECK_FALSE(assembler.Add(Message(fragments[0]), kNow));
  auto changed = fragments[0];
  changed[24] = 0xFF;
  CHECK_FALSE(assembler.Add(Message(changed), kNow));
  CHECK_FALSE(assembler.Add(Message(fragments[1]), kNow));
  CHECK(assembler.Stats().conflicting_frames == 1);
}

TEST_CASE("complete wire CRC error is rejected independently of source checksum") {
  gnss::Assembler assembler;
  auto fragments = Fragments(Sequence(41), 0x93EE);
  fragments[1][24] ^= 1;
  CHECK_FALSE(assembler.Add(Message(fragments[0]), kNow));
  CHECK_FALSE(assembler.Add(Message(fragments[1]), kNow));
  CHECK(assembler.Stats().wire_crc_errors == 1);
  CHECK(assembler.Stats().frames_completed == 0);
}

TEST_CASE("timeout is measured from the first fragment and duplicate cannot extend it") {
  gnss::Assembler assembler;
  const auto fragments = Fragments(Sequence(41), 0x93EE);
  CHECK_FALSE(assembler.Add(Message(fragments[0]), kNow));
  CHECK_FALSE(assembler.Add(Message(fragments[0]), kNow + 4999ms));
  assembler.Tick(kNow + 5s);
  CHECK(assembler.Stats().timed_out_frames == 1);
  CHECK_FALSE(assembler.Add(Message(fragments[1]), kNow + 5s));
  CHECK(assembler.Stats().frames_completed == 0);
}

TEST_CASE("completed keys suppress replay but allow reuse after five seconds") {
  gnss::Assembler assembler;
  const auto message = Message(Fragments(Sequence(1), 0xE1F0).front());
  CHECK(assembler.Add(message, kNow).has_value());
  CHECK_FALSE(assembler.Add(message, kNow + 4999ms));
  CHECK(assembler.Stats().duplicate_frames == 1);
  CHECK(assembler.Add(message, kNow + 5s).has_value());
}

TEST_CASE("bounded groups evict oldest and bounded tombstones release oldest keys") {
  gnss::Assembler assembler;
  for (std::uint32_t i = 0; i < 33; ++i) {
    const auto fragments = Fragments(Sequence(41), 0x93EE, 1, i);
    CHECK_FALSE(assembler.Add(Message(fragments[0]), kNow));
  }
  CHECK(assembler.Stats().evicted_frames == 1);
  CHECK_FALSE(assembler.Add(Message(Fragments(Sequence(41),0x93EE,1,0)[1]), kNow));
  CHECK(assembler.Add(Message(Fragments(Sequence(41),0x93EE,1,1)[1]), kNow).has_value());
  assembler.Reset();
  for (std::uint32_t i = 0; i < 65; ++i) {
    CHECK(assembler.Add(Message(Fragments(Sequence(1),0xE1F0,1,i).front()), kNow).has_value());
  }
  CHECK(assembler.Add(Message(Fragments(Sequence(1),0xE1F0,1,0).front()), kNow).has_value());
}

TEST_CASE("source and uptime isolate keys and Reset preserves lifetime counters") {
  gnss::Assembler assembler;
  auto fragments = Fragments(Sequence(41), 0x93EE);
  CHECK_FALSE(assembler.Add(Message(fragments[0]), kNow));
  CHECK_FALSE(assembler.Add(Pack(fragments[1],25,43), kNow));
  auto uptime = fragments[1];
  Put32(uptime,8,123);
  CHECK_FALSE(assembler.Add(Message(uptime), kNow));
  CHECK(assembler.Add(Message(fragments[1]), kNow).has_value());
  assembler.Reset();
  CHECK_FALSE(assembler.Add(Message(fragments[1]), kNow));
  CHECK(assembler.Add(Message(fragments[0]), kNow).has_value());
  CHECK(assembler.Stats().frames_completed == 2);
  CHECK(assembler.Stats().fragments_received == 6);
  CHECK(assembler.Stats().timed_out_frames == 0);
}
