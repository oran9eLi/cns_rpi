#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bmi088_fixture.hpp"
#include "imu/bmi088.hpp"

using namespace bmi_fixture;

TEST_CASE("wire fixtures decode with official MAVLink tail-zero restoration") {
  for (int kind = 0; kind < 3; ++kind) {
    const auto bytes = Payload(kind);
    const auto message = Message(bytes, kind);
    CHECK(message.len < MAVLINK_MSG_ID_TUNNEL_LEN);
    auto decoded = imu::Decode(message);
    REQUIRE(decoded);
    REQUIRE(decoded->has_value());
    const auto& packet = **decoded;
    CHECK(packet.system_id == 218);
    CHECK(packet.component_id == 193);
    CHECK(packet.header.sequence == 42);
    CHECK(packet.header.sample_time == 10000);
    CHECK(packet.header.reinit_count == 1);
    CHECK(std::equal(packet.payload.begin(), packet.payload.end(), bytes.begin()));
    if (kind == 0) {
      const auto& raw = std::get<imu::Raw>(packet.data);
      CHECK(raw.accel == std::array<std::int16_t, 3>{0, 0, 2731});
      CHECK(raw.gyro == std::array<std::int16_t, 3>{0, 0, 0});
      CHECK(raw.temperature_cdeg == 2300);
    } else if (kind == 1) {
      const auto& parsed = std::get<imu::Parsed>(packet.data);
      CHECK(parsed.accel_mg == std::array<std::int32_t, 3>{0, 0, 1000});
      CHECK(parsed.axis_map == std::array<std::int8_t, 3>{1, 2, 3});
      CHECK(parsed.sample_period_us == 10000);
    } else {
      const auto& status = std::get<imu::Status>(packet.data);
      CHECK(status.completed_pairs == 3);
      CHECK(status.source_overwrites == 39);
      CHECK(status.device_ready == 1);
    }
  }
}

TEST_CASE("signed wire integers, last FIFO frame, and temperature low bits") {
  auto raw = Payload(0);
  raw[28] = 0; raw[29] = 0x80;
  raw[25] = 2; raw[26] = 2;
  raw[45] = 0xff; raw[46] = 0xff;
  raw[37] = 0xff; raw[38] = 0xff;
  auto decoded = imu::Decode(Message(raw, 0));
  REQUIRE((decoded && decoded->has_value()));
  const auto& value = std::get<imu::Raw>((**decoded).data);
  CHECK(value.accel[0] == -32768);
  CHECK(value.gyro[0] == -1);
  CHECK(value.temperature_cdeg == doctest::Approx(2287.5));
  auto parsed = Payload(1);
  Put32(parsed, 20, static_cast<std::uint32_t>(-12000));
  parsed[44] = 0x18; parsed[45] = 0xfc;
  parsed[52] = 2; parsed[53] = 255;
  decoded = imu::Decode(Message(parsed, 1));
  REQUIRE((decoded && decoded->has_value()));
  CHECK(std::get<imu::Parsed>((**decoded).data).accel_mg[0] == -12000);
  CHECK(std::get<imu::Parsed>((**decoded).data).temperature_cdeg == -1000);
}

TEST_CASE("SPI raw and unknown status preserve their defined fields") {
  auto raw = Payload(0);
  raw[3] = 2; raw[20] = raw[21] = 0;
  CHECK(imu::Decode(Message(raw, 0))->has_value());
  auto status = Payload(2);
  status[2] = 0; status[3] = 0; status[51] = 0; status[52] = 0xab;
  status[60] = 0; status[61] = 0; status[62] = 255;
  Put32(status, 56, 0xffffffff);
  auto decoded = imu::Decode(Message(status, 2));
  REQUIRE((decoded && decoded->has_value()));
  CHECK(std::get<imu::Status>((**decoded).data).accel_chip_id == 0xab);
}

TEST_CASE("non BMI frames are ignored and malformed BMI entry is rejected") {
  mavlink_message_t other{};
  CHECK_FALSE(imu::Decode(other)->has_value());
  CHECK_FALSE(imu::Decode(Message(Payload(0), 5))->has_value());
  CHECK_FALSE(imu::Decode(Message(Payload(0), 0, 218, 193, 63)));
  CHECK_FALSE(imu::Decode(Message(Payload(0), 0, 218, 193, 64, 1)));
  CHECK_FALSE(imu::Decode(Message(Payload(0), 0, 0)));
  CHECK_FALSE(imu::Decode(Message(Payload(0), 0, 218, 1)));
  for (const auto& [offset, value] : std::array<std::pair<int, int>, 13>{
      {{0, 2}, {1, 1}, {2, 19}, {3, 0}, {20, 0}, {22, 0}, {24, 0},
       {25, 129}, {26, 0}, {27, 2}, {37, 128}, {62, 1}, {64, 1}}}) {
    auto bytes = Payload(0); bytes[offset] = value;
    INFO("RAW offset ", offset);
    CHECK_FALSE(imu::Decode(Message(bytes, 0)));
  }
  for (const auto& [offset, value] : std::array<std::pair<int, int>, 6>{
      {{48, 0}, {50, 0}, {52, 0}, {53, 1}, {54, 253}, {55, 2}}}) {
    auto bytes = Payload(1); bytes[offset] = value;
    INFO("PARSED offset ", offset);
    CHECK_FALSE(imu::Decode(Message(bytes, 1)));
  }
  for (const auto& [offset, value] : std::array<std::pair<int, int>, 8>{
      {{48, 10}, {49, 7}, {50, 4}, {51, 2}, {60, 2}, {61, 2}, {62, 7}, {63, 1}}}) {
    auto bytes = Payload(2); bytes[offset] = value;
    INFO("STATUS offset ", offset);
    CHECK_FALSE(imu::Decode(Message(bytes, 2)));
  }
  auto bytes = Payload(0); Put32(bytes, 12, 10201);
  CHECK_FALSE(imu::Decode(Message(bytes, 0)));
}

TEST_CASE("sequence zero is reserved for no sample, while sample time zero is valid") {
  for (int kind = 0; kind < 3; ++kind) {
    auto bytes = Payload(kind);
    Put32(bytes, 4, 0);
    CHECK_FALSE(imu::Decode(Message(bytes, kind)));
  }
  auto empty = Payload(2);
  empty[2] = 0; empty[61] = 0;
  Put32(empty, 4, 0); Put32(empty, 8, 0); Put32(empty, 56, 0xffffffff);
  auto decoded = imu::Decode(Message(empty, 2));
  REQUIRE((decoded && decoded->has_value()));
  auto at_boot = Payload(0);
  Put32(at_boot, 4, 1); Put32(at_boot, 8, 0); Put32(at_boot, 12, 5);
  decoded = imu::Decode(Message(at_boot, 0));
  REQUIRE((decoded && decoded->has_value()));
}
