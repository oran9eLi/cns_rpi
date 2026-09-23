#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <nlohmann/json.hpp>

#include "experiment/uart_echo_protocol.hpp"
#include "experiment/uart_echo_transaction.hpp"

namespace {
using Clock = std::chrono::steady_clock;
using Wall = std::chrono::system_clock;

experiment::UartEchoRequest Request(Wall::time_point wall) {
  return {.command_id = "11111111-1111-4111-8111-111111111111",
          .request_id = "22222222-2222-4222-8222-222222222222",
          .device_id = "DCDWCNS1GHC0G6LF8MY6",
          .session_id = "33333333-3333-4333-8333-333333333333",
          .action_id = "22222222-2222-4222-8222-222222222222",
          .lease_version = 1,
          .expires_at = "2026-09-23T10:00:00.000Z",
          .expiry = wall + std::chrono::minutes(1),
          .text_bytes = {'A', 0xE4, 0xB8, 0xAD}};
}

experiment::EchoGate Gate() {
  return {.serial_open = true, .identity_verified = true, .cns_box = true,
          .serial_busy = false, .source_system = 42, .source_component = 191,
          .target_system = 43, .target_component = 193, .pi_baud = 115200};
}

std::filesystem::path Journal() {
  static int serial = 0;
  return std::filesystem::temp_directory_path() /
         ("cns-h1-journal-test-" + std::to_string(++serial) + ".json");
}

mavlink_message_t Response(std::uint64_t nonce, int baud, std::uint8_t compid = 193) {
  std::uint8_t payload[128]{};
  payload[0] = 1; payload[1] = 2;
  for (int i = 0; i < 8; ++i) payload[2+i] = static_cast<std::uint8_t>(nonce >> (8*i));
  payload[10] = 4; payload[11] = 'A'; payload[12] = 0xE4;
  payload[13] = 0xB8; payload[14] = 0xAD;
  for (int i = 0; i < 4; ++i) payload[15+i] = static_cast<std::uint8_t>(baud >> (8*i));
  mavlink_message_t message{};
  mavlink_msg_tunnel_pack(43, compid, &message, 42, 191, 0x8003, 19, payload);
  return message;
}

uart::WireFrame Wire(mavlink_message_t message, Clock::time_point at) {
  std::uint8_t raw[MAVLINK_MAX_PACKET_LEN]{};
  auto length = mavlink_msg_to_send_buffer(raw, &message);
  return {.message = message, .bytes = {raw, raw + length}, .received_at = at};
}
}  // namespace

TEST_CASE("H1 完整回显与本动作后新业务帧共同形成成功事实") {
  auto path = Journal();
  const auto wall = Wall::time_point{std::chrono::seconds(1000)};
  const auto t0 = Clock::time_point{std::chrono::seconds(100)};
  experiment::UartEchoTransaction tx(path, "cns", [] {
    return std::expected<std::uint64_t, std::string>{0x0102030405060708ULL};
  });
  REQUIRE(tx.Load().has_value());
  auto started = tx.Start(Request(wall), Gate(), wall, t0);
  REQUIRE(started.outbound);
  CHECK_FALSE(started.publication);
  auto sent = Wire(*started.outbound, t0);
  CHECK_FALSE(tx.OnSent({sent.bytes, t0}, wall));
  CHECK_FALSE(tx.OnBusinessFrame(t0 - std::chrono::milliseconds(1), wall));
  auto response = Wire(Response(0x0102030405060708ULL, 115200),
                       t0 + std::chrono::milliseconds(3));
  CHECK_FALSE(tx.OnFrame(response, wall));
  auto ack = tx.OnBusinessFrame(t0 + std::chrono::milliseconds(4), wall);
  REQUIRE(ack);
  auto body = nlohmann::json::parse(ack->payload);
  CHECK(body.at("status") == "completed");
  CHECK(body.at("fact").at("request_text") == "A中");
  CHECK(body.at("fact").at("response_text") == "A中");
  CHECK(body.at("fact").at("fresh_business_frame") == true);
  CHECK(body.at("fact").at("round_trip_us") == 3000);
  CHECK(body.at("fact").at("pi_baud_rate") == 115200);
  CHECK(body.at("fact").at("f407_baud_rate") == 115200);
  CHECK(body.at("fact").at("tx_frame_bytes") == sent.bytes.size());
  CHECK(body.at("fact").at("rx_frame_bytes") == response.bytes.size());
  CHECK(body.at("fact").at("tx_frame_hex") == experiment::FrameHex(sent.bytes));
  CHECK(body.at("fact").at("rx_frame_hex") == experiment::FrameHex(response.bytes));
  CHECK(ack->qos == 2);
  CHECK_FALSE(ack->retain);
  CHECK_FALSE(tx.HasPending());
  std::filesystem::remove(path);
}

TEST_CASE("H1 缓存身份、错误来源与波特率不一致均不得成功") {
  auto path = Journal();
  const auto wall = Wall::time_point{std::chrono::seconds(1000)};
  const auto t0 = Clock::time_point{std::chrono::seconds(100)};
  experiment::UartEchoTransaction tx(path, "cns", [] {
    return std::expected<std::uint64_t, std::string>{123};
  });
  REQUIRE(tx.Load());
  auto gate = Gate(); gate.identity_verified = false;
  auto denied = tx.Start(Request(wall), gate, wall, t0);
  REQUIRE(denied.publication);
  CHECK(nlohmann::json::parse(denied.publication->payload).at("status") == "rejected");
  CHECK_FALSE(denied.outbound);
  auto started = tx.Start(Request(wall), Gate(), wall, t0);
  REQUIRE(started.outbound);
  auto sent = Wire(*started.outbound, t0);
  CHECK_FALSE(tx.OnSent({sent.bytes, t0}, wall));
  CHECK_FALSE(tx.OnFrame(Wire(Response(123, 115200, 194), t0 + std::chrono::milliseconds(1)), wall));
  auto ack = tx.OnFrame(Wire(Response(123, 57600), t0 + std::chrono::milliseconds(2)), wall);
  REQUIRE(ack);
  CHECK(nlohmann::json::parse(ack->payload).at("error_code") == "baud_mismatch");
  CHECK_FALSE(nlohmann::json::parse(ack->payload).contains("fact"));
  std::filesystem::remove(path);
}

TEST_CASE("H1 发帧前记录动作，崩溃重启后拒绝重复物理发送") {
  auto path = Journal();
  const auto wall = Wall::time_point{std::chrono::seconds(1000)};
  const auto t0 = Clock::time_point{std::chrono::seconds(100)};
  {
    experiment::UartEchoTransaction tx(path, "cns", [] {
      return std::expected<std::uint64_t, std::string>{123};
    });
    REQUIRE(tx.Load());
    REQUIRE(tx.Start(Request(wall), Gate(), wall, t0).outbound);
  }
  experiment::UartEchoTransaction restarted(path, "cns");
  REQUIRE(restarted.Load());
  auto replay = restarted.Start(Request(wall), Gate(), wall, t0);
  CHECK_FALSE(replay.outbound);
  REQUIRE(replay.publication);
  CHECK(nlohmann::json::parse(replay.publication->payload).at("error_code") == "result_uncertain");
  auto conflict_request = Request(wall); conflict_request.text_bytes = {'B'};
  auto conflict = restarted.Start(conflict_request, Gate(), wall, t0);
  REQUIRE(conflict.publication);
  CHECK(nlohmann::json::parse(conflict.publication->payload).at("error_code") == "duplicate_conflict");
  std::filesystem::remove(path);
}

TEST_CASE("H1 错误nonce、无新业务帧和等待超时仅返回受限拒绝") {
  const auto wall = Wall::time_point{std::chrono::seconds(1000)};
  const auto t0 = Clock::time_point{std::chrono::seconds(100)};
  for (int mode = 0; mode < 3; ++mode) {
    auto path = Journal();
    experiment::UartEchoTransaction tx(path, "cns", [] {
      return std::expected<std::uint64_t, std::string>{123};
    });
    REQUIRE(tx.Load());
    auto started = tx.Start(Request(wall), Gate(), wall, t0);
    REQUIRE(started.outbound);
    auto sent = Wire(*started.outbound, t0);
    CHECK_FALSE(tx.OnSent({sent.bytes, t0}, wall));
    std::optional<experiment::Publication> ack;
    if (mode == 0) {
      ack = tx.OnFrame(Wire(Response(124, 115200), t0 + std::chrono::milliseconds(2)), wall);
    } else if (mode == 1) {
      CHECK_FALSE(tx.OnFrame(Wire(Response(123, 115200), t0 + std::chrono::milliseconds(2)), wall));
      ack = tx.Tick(t0 + std::chrono::seconds(10), wall);
    } else {
      ack = tx.Tick(t0 + std::chrono::seconds(5), wall);
    }
    REQUIRE(ack);
    auto body = nlohmann::json::parse(ack->payload);
    CHECK(body.at("status") == "rejected");
    CHECK_FALSE(body.contains("fact"));
    CHECK(body.at("error_code") == (mode == 0 ? "nonce_mismatch" :
                                     mode == 1 ? "fresh_business_frame_missing" :
                                                 "f407_timeout"));
    std::filesystem::remove(path);
  }
}

TEST_CASE("H1 已完成动作重投只重发终态ACK") {
  const auto wall = Wall::time_point{std::chrono::seconds(1000)};
  const auto t0 = Clock::time_point{std::chrono::seconds(100)};
  auto path = Journal();
  experiment::UartEchoTransaction tx(path, "cns", [] {
    return std::expected<std::uint64_t, std::string>{123};
  });
  REQUIRE(tx.Load());
  auto started = tx.Start(Request(wall), Gate(), wall, t0);
  REQUIRE(started.outbound);
  auto sent = Wire(*started.outbound, t0);
  CHECK_FALSE(tx.OnSent({sent.bytes, t0}, wall));
  CHECK_FALSE(tx.OnFrame(Wire(Response(123, 115200), t0 + std::chrono::milliseconds(2)), wall));
  auto completed = tx.OnBusinessFrame(t0 + std::chrono::milliseconds(3), wall);
  REQUIRE(completed);
  auto duplicate = tx.Start(Request(wall), Gate(), wall, t0);
  CHECK_FALSE(duplicate.outbound);
  REQUIRE(duplicate.publication);
  CHECK(duplicate.publication->payload == completed->payload);
  std::filesystem::remove(path);
}
