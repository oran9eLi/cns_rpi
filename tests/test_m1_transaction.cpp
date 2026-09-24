#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <nlohmann/json.hpp>

#include "experiment/m1_transaction.hpp"

namespace {
using Clock = std::chrono::steady_clock;
using Wall = std::chrono::system_clock;
constexpr std::uint64_t kNonce = 0x0102030405060708ULL;

experiment::M1Request Request(experiment::M1Operation operation,
                              Wall::time_point wall) {
  return {.command_id = "11111111-1111-4111-8111-111111111111",
          .request_id = "22222222-2222-4222-8222-222222222222",
          .device_id = "DCDWCNS1GHC0G6LF8MY6",
          .session_id = "33333333-3333-4333-8333-333333333333",
          .action_id = "22222222-2222-4222-8222-222222222222",
          .lease_version = 1,
          .operation = operation,
          .baud_rate = operation == experiment::M1Operation::kUartProbe
                           ? std::nullopt : std::optional<int>{57600},
          .expires_at = "2026-09-24T10:00:30.000Z",
          .expiry = wall + std::chrono::minutes(1)};
}

experiment::M1Gate Gate() {
  return {.cns_box = true, .persisted_binding = true,
          .identity_verified = true, .trusted_endpoint = true,
          .serial_open = true, .serial_busy = false, .mqtt_connected = true,
          .source_system = 42, .source_component = 191,
          .target_system = 43, .target_component = 193, .pi_baud = 115200};
}

std::filesystem::path Journal() {
  static int serial = 0;
  return std::filesystem::temp_directory_path() /
         ("cns-m1-journal-test-" + std::to_string(++serial) + ".json");
}

uart::WireFrame Wire(const mavlink_message_t& message, Clock::time_point received_at) {
  std::uint8_t raw[MAVLINK_MAX_PACKET_LEN]{};
  const auto length = mavlink_msg_to_send_buffer(raw, &message);
  return {.message = message, .bytes = {raw, raw + length},
          .received_at = received_at};
}

mavlink_message_t Accepted(std::uint64_t nonce = kNonce,
                           std::uint32_t baud = 57600,
                           std::uint8_t component = 193) {
  std::uint8_t payload[128]{};
  payload[0] = 1; payload[1] = 4;
  for (int i = 0; i < 8; ++i) payload[2 + i] = static_cast<std::uint8_t>(nonce >> (8 * i));
  for (int i = 0; i < 4; ++i) payload[10 + i] = static_cast<std::uint8_t>(baud >> (8 * i));
  mavlink_message_t message{};
  mavlink_msg_tunnel_pack(43, component, &message, 42, 191, 0x8003, 14, payload);
  return message;
}

mavlink_message_t Echo(std::uint64_t nonce, std::uint32_t baud = 115200) {
  std::uint8_t payload[128]{};
  payload[0] = 1; payload[1] = 2;
  for (int i = 0; i < 8; ++i) payload[2 + i] = static_cast<std::uint8_t>(nonce >> (8 * i));
  payload[10] = 8;
  const char text[] = "CNS_TEST";
  for (int i = 0; i < 8; ++i) payload[11 + i] = static_cast<std::uint8_t>(text[i]);
  for (int i = 0; i < 4; ++i) payload[19 + i] = static_cast<std::uint8_t>(baud >> (8 * i));
  mavlink_message_t message{};
  mavlink_msg_tunnel_pack(43, 193, &message, 42, 191, 0x8003, 23, payload);
  return message;
}
}  // namespace

TEST_CASE("F407改参只记录旧速率接受事实且重复动作不重发") {
  const auto path = Journal();
  const auto wall = Wall::time_point{std::chrono::seconds(1000)};
  const auto t0 = Clock::time_point{std::chrono::seconds(100)};
  const auto request = Request(experiment::M1Operation::kSetF407Baud, wall);
  experiment::M1Transaction tx(path, "cns", [] {
    return std::expected<std::uint64_t, std::string>{kNonce};
  });
  REQUIRE(tx.Load().has_value());
  const auto started = tx.Start(request, Gate(), wall, t0);
  REQUIRE(started.outbound.has_value());
  CHECK_FALSE(started.publication.has_value());
  const auto sent = Wire(*started.outbound, t0);
  CHECK_FALSE(tx.OnSent({sent.bytes, t0}, wall).publication.has_value());
  const auto response = Wire(Accepted(), t0 + std::chrono::milliseconds(7));
  const auto result = tx.OnFrame(response, wall);
  REQUIRE(result.publication.has_value());
  const auto body = nlohmann::json::parse(result.publication->payload);
  CHECK(body.at("status") == "completed");
  CHECK(body.at("fact").at("accepted_baud_rate") == 57600);
  CHECK(body.at("fact").at("pi_baud_rate") == 115200);
  CHECK(body.at("fact").at("nonce") == "0102030405060708");
  CHECK(body.at("fact").at("tx_frame_bytes") == sent.bytes.size());
  CHECK(body.at("fact").at("rx_frame_bytes") == response.bytes.size());
  CHECK_FALSE(body.at("fact").contains("f407_baud_rate"));
  CHECK(result.publication->qos == 2);
  CHECK_FALSE(result.publication->retain);
  const auto duplicate = tx.Start(request, Gate(), wall, t0);
  CHECK_FALSE(duplicate.outbound.has_value());
  REQUIRE(duplicate.publication.has_value());
  CHECK(duplicate.publication->payload == result.publication->payload);
  std::filesystem::remove(path);
}

TEST_CASE("已发F407改参帧无接受响应时结果不确定且重启不重发") {
  const auto path = Journal();
  const auto wall = Wall::time_point{std::chrono::seconds(1000)};
  const auto t0 = Clock::time_point{std::chrono::seconds(100)};
  const auto request = Request(experiment::M1Operation::kSetF407Baud, wall);
  {
    experiment::M1Transaction tx(path, "cns", [] {
      return std::expected<std::uint64_t, std::string>{kNonce};
    });
    REQUIRE(tx.Load().has_value());
    const auto started = tx.Start(request, Gate(), wall, t0);
    REQUIRE(started.outbound.has_value());
    CHECK_FALSE(tx.OnSent({Wire(*started.outbound, t0).bytes, t0}, wall).publication);
    CHECK_FALSE(tx.OnFrame(Wire(Accepted(kNonce + 1), t0 + std::chrono::seconds(1)), wall).publication);
    const auto timeout = tx.Tick(t0 + std::chrono::seconds(5), wall);
    REQUIRE(timeout.publication.has_value());
    CHECK(nlohmann::json::parse(timeout.publication->payload).at("error_code") ==
          "result_uncertain");
  }
  experiment::M1Transaction restarted(path, "cns");
  REQUIRE(restarted.Load().has_value());
  const auto duplicate = restarted.Start(request, Gate(), wall, t0);
  CHECK_FALSE(duplicate.outbound.has_value());
  REQUIRE(duplicate.publication.has_value());
  CHECK(nlohmann::json::parse(duplicate.publication->payload).at("error_code") ==
        "result_uncertain");
  std::filesystem::remove(path);
}

TEST_CASE("失配探测三次真实发送而无有效回显时仅形成观察事实") {
  const auto path = Journal();
  const auto wall = Wall::time_point{std::chrono::seconds(1000)};
  const auto t0 = Clock::time_point{std::chrono::seconds(100)};
  std::uint64_t next_nonce = kNonce;
  experiment::M1Transaction tx(path, "cns", [&] {
    return std::expected<std::uint64_t, std::string>{next_nonce++};
  });
  REQUIRE(tx.Load().has_value());
  auto gate = Gate(); gate.identity_verified = false;
  const auto request = Request(experiment::M1Operation::kUartProbe, wall);
  auto started = tx.Start(request, gate, wall, t0);
  REQUIRE(started.outbound.has_value());
  for (int round = 0; round < 3; ++round) {
    const auto sent_at = t0 + std::chrono::seconds(round * 5);
    const auto sent = Wire(*started.outbound, sent_at);
    CHECK_FALSE(tx.OnSent({sent.bytes, sent_at}, wall).publication.has_value());
    const auto step = tx.Tick(sent_at + std::chrono::seconds(5), wall);
    if (round < 2) {
      REQUIRE(step.outbound.has_value());
      started.outbound = step.outbound;
    } else {
      REQUIRE(step.publication.has_value());
      const auto body = nlohmann::json::parse(step.publication->payload);
      CHECK(body.at("status") == "completed");
      CHECK(body.at("fact").at("attempted_count") == 3);
      CHECK(body.at("fact").at("valid_response_count") == 0);
      REQUIRE(body.at("fact").at("samples").size() == 3);
      for (const auto& sample : body.at("fact").at("samples")) {
        CHECK(sample.at("valid_rx_frame_hex").is_null());
        CHECK(sample.at("rx_frame_bytes").is_null());
        CHECK(sample.at("round_trip_us").is_null());
        CHECK(sample.at("tx_frame_bytes").get<int>() > 0);
      }
    }
  }
  std::filesystem::remove(path);
}

TEST_CASE("Pi在线改参先占用动作身份，应用完成后才给成功事实") {
  const auto path = Journal();
  const auto wall = Wall::time_point{std::chrono::seconds(1000)};
  const auto t0 = Clock::time_point{std::chrono::seconds(100)};
  experiment::M1Transaction tx(path, "cns");
  REQUIRE(tx.Load().has_value());
  auto gate = Gate(); gate.identity_verified = false; gate.serial_open = false;
  const auto request = Request(experiment::M1Operation::kSetPiBaud, wall);
  const auto started = tx.Start(request, gate, wall, t0);
  CHECK_FALSE(started.outbound.has_value());
  REQUIRE(started.apply_pi_baud.has_value());
  CHECK(*started.apply_pi_baud == 57600);
  CHECK_FALSE(started.publication.has_value());
  const auto done = tx.OnPiBaudApplied(57600, true, wall);
  REQUIRE(done.has_value());
  const auto body = nlohmann::json::parse(done->payload);
  CHECK(body.at("status") == "completed");
  CHECK(body.at("fact").at("pi_baud_rate") == 57600);
  CHECK(body.at("fact").at("serial_port_open") == true);
  std::filesystem::remove(path);
}

TEST_CASE("探测只计本轮配对回显且两次发送至少相隔300毫秒") {
  const auto path = Journal();
  const auto wall = Wall::time_point{std::chrono::seconds(1000)};
  const auto t0 = Clock::time_point{std::chrono::seconds(100)};
  std::uint64_t next_nonce = kNonce;
  experiment::M1Transaction tx(path, "cns", [&] {
    return std::expected<std::uint64_t, std::string>{next_nonce++};
  });
  REQUIRE(tx.Load().has_value());
  auto gate = Gate(); gate.identity_verified = false;
  const auto started = tx.Start(Request(experiment::M1Operation::kUartProbe, wall),
                                gate, wall, t0);
  REQUIRE(started.outbound.has_value());
  CHECK_FALSE(tx.OnSent({Wire(*started.outbound, t0).bytes, t0}, wall).publication);
  CHECK_FALSE(tx.OnFrame(Wire(Echo(kNonce + 1), t0 + std::chrono::milliseconds(5)), wall).publication);
  CHECK_FALSE(tx.OnFrame(Wire(Echo(kNonce), t0 + std::chrono::milliseconds(10)), wall).publication);
  CHECK_FALSE(tx.Tick(t0 + std::chrono::milliseconds(299), wall).outbound);

  const auto second = tx.Tick(t0 + std::chrono::milliseconds(300), wall);
  REQUIRE(second.outbound.has_value());
  const auto second_at = t0 + std::chrono::milliseconds(300);
  CHECK_FALSE(tx.OnSent({Wire(*second.outbound, second_at).bytes, second_at}, wall).publication);
  const auto third = tx.Tick(second_at + std::chrono::seconds(5), wall);
  REQUIRE(third.outbound.has_value());
  const auto third_at = second_at + std::chrono::seconds(5);
  CHECK_FALSE(tx.OnSent({Wire(*third.outbound, third_at).bytes, third_at}, wall).publication);
  CHECK_FALSE(tx.OnFrame(Wire(Echo(kNonce), third_at + std::chrono::milliseconds(1)), wall).publication);
  CHECK_FALSE(tx.OnFrame(Wire(Echo(kNonce + 2), third_at + std::chrono::milliseconds(10)), wall).publication);
  const auto result = tx.Tick(third_at + std::chrono::milliseconds(300), wall);
  REQUIRE(result.publication.has_value());
  const auto body = nlohmann::json::parse(result.publication->payload);
  CHECK(body.at("fact").at("valid_response_count") == 2);
  const auto& samples = body.at("fact").at("samples");
  CHECK(samples.at(0).at("round_trip_us") == 10000);
  CHECK(samples.at(1).at("valid_rx_frame_hex").is_null());
  CHECK(samples.at(1).at("rx_frame_bytes").is_null());
  CHECK(samples.at(1).at("round_trip_us").is_null());
  CHECK(samples.at(2).at("round_trip_us") == 10000);
  std::filesystem::remove(path);
}

TEST_CASE("Pi改参动作重启后不再次写配置且变更参数构成冲突") {
  const auto path = Journal();
  const auto wall = Wall::time_point{std::chrono::seconds(1000)};
  const auto request = Request(experiment::M1Operation::kSetPiBaud, wall);
  {
    experiment::M1Transaction tx(path, "cns");
    REQUIRE(tx.Load().has_value());
    const auto first = tx.Start(request, Gate(), wall, Clock::time_point{});
    REQUIRE(first.apply_pi_baud.has_value());
    CHECK(tx.HasActionId(request.action_id));
  }
  experiment::M1Transaction restarted(path, "cns");
  REQUIRE(restarted.Load().has_value());
  const auto duplicate = restarted.Start(request, Gate(), wall, Clock::time_point{});
  CHECK_FALSE(duplicate.apply_pi_baud.has_value());
  REQUIRE(duplicate.publication.has_value());
  CHECK(nlohmann::json::parse(duplicate.publication->payload).at("error_code") ==
        "result_uncertain");
  auto changed = request; changed.baud_rate = 115200;
  const auto conflict = restarted.Start(changed, Gate(), wall, Clock::time_point{});
  CHECK_FALSE(conflict.apply_pi_baud.has_value());
  REQUIRE(conflict.publication.has_value());
  CHECK(nlohmann::json::parse(conflict.publication->payload).at("error_code") ==
        "duplicate_conflict");
  std::filesystem::remove(path);
}

TEST_CASE("无可信端点不能把探测或本地改参伪装为已完成") {
  const auto path = Journal();
  const auto wall = Wall::time_point{std::chrono::seconds(1000)};
  experiment::M1Transaction tx(path, "cns");
  REQUIRE(tx.Load().has_value());
  auto gate = Gate(); gate.trusted_endpoint = false;
  const auto probe = tx.Start(Request(experiment::M1Operation::kUartProbe, wall),
                              gate, wall, Clock::time_point{});
  CHECK_FALSE(probe.outbound.has_value());
  REQUIRE(probe.publication.has_value());
  CHECK(nlohmann::json::parse(probe.publication->payload).at("error_code") ==
        "serial_unavailable");
  auto pi_request = Request(experiment::M1Operation::kSetPiBaud, wall);
  pi_request.action_id = "44444444-4444-4444-8444-444444444444";
  pi_request.request_id = pi_request.action_id;
  const auto pi = tx.Start(pi_request, gate, wall, Clock::time_point{});
  CHECK_FALSE(pi.apply_pi_baud.has_value());
  REQUIRE(pi.publication.has_value());
  CHECK(nlohmann::json::parse(pi.publication->payload).at("error_code") ==
        "serial_unavailable");
  std::filesystem::remove(path);
}

TEST_CASE("事务入口不会因缺失改参目标值而解引用空值") {
  const auto path = Journal();
  const auto wall = Wall::time_point{std::chrono::seconds(1000)};
  experiment::M1Transaction tx(path, "cns");
  REQUIRE(tx.Load().has_value());
  auto request = Request(experiment::M1Operation::kSetF407Baud, wall);
  request.baud_rate.reset();
  const auto started = tx.Start(request, Gate(), wall, Clock::time_point{});
  CHECK_FALSE(started.outbound.has_value());
  REQUIRE(started.publication.has_value());
  CHECK(nlohmann::json::parse(started.publication->payload).at("status") ==
        "rejected");
  std::filesystem::remove(path);
}
