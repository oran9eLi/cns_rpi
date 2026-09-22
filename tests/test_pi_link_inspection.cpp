#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <array>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "experiment/pi_link_inspection.hpp"

namespace {

using namespace std::chrono_literals;
constexpr auto kWallNow = std::chrono::sys_days{std::chrono::year{2026}/9/22} +
                          9h + 2s + 123ms;
constexpr auto kSteadyNow = std::chrono::steady_clock::time_point{300s};
constexpr const char* kDeviceId = "DCDWCNS1S2MEAG1VTA0C";
constexpr const char* kSetTopic = "cns_rpi/DCDWCNS1S2MEAG1VTA0C/experiment/set";
constexpr const char* kAckTopic = "cns_rpi/DCDWCNS1S2MEAG1VTA0C/experiment/ack";
constexpr const char* kCommandId = "7d15e276-c7e1-4f52-aaee-7314a6417667";
constexpr const char* kActionId = "f886f45b-01f8-4a54-b2b6-a65b07477aa1";
constexpr const char* kSessionId = "c4191037-9e2a-480c-a995-2174e4672563";

nlohmann::json Request() {
  return {{"schema_version", 1}, {"command_id", kCommandId},
          {"request_id", kActionId}, {"target", {{"device_id", kDeviceId}}},
          {"session_id", kSessionId}, {"action_id", kActionId},
          {"lease_version", 3}, {"operation", "inspect_pi_link"},
          {"expires_at", "2026-09-22T09:00:30.000Z"}};
}

std::optional<device::Binding> Bound() {
  return device::Binding{.device_id = kDeviceId,
                         .device_type = device::Type::kCnsBox};
}

experiment::Observation Observed(
    runtime_status::IdentityStatus identity = runtime_status::IdentityStatus::kCached,
    runtime_status::BusinessStatus business = runtime_status::BusinessStatus::kOffline,
    std::optional<bool> serial = false) {
  return {.runtime = {.device_id = kDeviceId,
                      .control_status = runtime_status::ControlStatus::kOnline,
                      .business_status = business,
                      .identity_status = identity},
          .serial_port_open = serial,
          .mqtt_connected = true};
}

experiment::Outcome Handle(experiment::PiLinkInspector& inspector,
                           const nlohmann::json& request,
                           const experiment::PiLinkInspector::Sampler& sampler,
                           std::chrono::system_clock::time_point wall = kWallNow,
                           std::chrono::steady_clock::time_point steady = kSteadyNow) {
  return inspector.Handle(kSetTopic, request.dump(), Bound(), sampler, wall, steady);
}

nlohmann::json Ack(const experiment::Outcome& outcome) {
  REQUIRE(outcome.publication.has_value());
  CHECK(outcome.publication->topic == kAckTopic);
  CHECK(outcome.publication->qos == 2);
  CHECK_FALSE(outcome.publication->retain);
  CHECK(outcome.publication->payload.size() <= 4096);
  return nlohmann::json::parse(outcome.publication->payload);
}

}  // namespace

TEST_CASE("缓存身份和业务离线仍完成只读Pi自检") {
  experiment::PiLinkInspector inspector("cns_rpi");
  int sampled = 0;
  const auto ack = Ack(Handle(inspector, Request(), [&] {
    ++sampled;
    return Observed();
  }));
  CHECK(sampled == 1);
  CHECK(ack == nlohmann::json{
      {"schema_version", 1}, {"command_id", kCommandId},
      {"session_id", kSessionId}, {"action_id", kActionId},
      {"operation", "inspect_pi_link"}, {"status", "completed"},
      {"observed_at", "2026-09-22T09:00:02.123Z"},
      {"fact", {{"control_status", "online"}, {"identity_status", "cached"},
                {"business_status", "offline"}, {"serial_port_open", false}}}});
}

TEST_CASE("仅持久化主控箱身份订阅固定实验Topic与QoS2") {
  const auto subscription = experiment::DeviceSubscription("cns_rpi", Bound());
  REQUIRE(subscription.has_value());
  CHECK(subscription->first == kSetTopic);
  CHECK(subscription->second == 2);
  CHECK_FALSE(experiment::DeviceSubscription("cns_rpi", std::nullopt));
  CHECK_FALSE(experiment::DeviceSubscription(
      "cns_rpi", device::Binding{.device_id = "PX4ID",
                                   .device_type = device::Type::kFlightController}));
}

TEST_CASE("串口状态不能确认时原样返回unknown") {
  experiment::PiLinkInspector inspector("cns_rpi");
  const auto ack = Ack(Handle(inspector, Request(), [] {
    return Observed(runtime_status::IdentityStatus::kVerified,
                    runtime_status::BusinessStatus::kOnline, std::nullopt);
  }));
  CHECK(ack.at("fact").at("serial_port_open") == "unknown");
}

TEST_CASE("串口自动发现中不把候选端点误报为关闭") {
  CHECK(experiment::SerialEndpointFact(true) == true);
  CHECK_FALSE(experiment::SerialEndpointFact(false).has_value());
}

TEST_CASE("身份冲突与未绑定拒绝且不携带事实") {
  for (auto identity : {runtime_status::IdentityStatus::kConflict,
                        runtime_status::IdentityStatus::kUnbound}) {
    experiment::PiLinkInspector inspector("cns_rpi");
    const auto ack = Ack(Handle(inspector, Request(), [=] { return Observed(identity); }));
    CHECK(ack.at("status") == "rejected");
    CHECK(ack.at("error_code") == "identity_unavailable");
    CHECK_FALSE(ack.contains("fact"));
  }
}

TEST_CASE("同内容重投复用原事实时间并只替换合法命令号") {
  experiment::PiLinkInspector inspector("cns_rpi");
  int sampled = 0;
  const auto first = Ack(Handle(inspector, Request(), [&] {
    ++sampled;
    return Observed();
  }));
  auto retry = Request();
  retry["command_id"] = "f77e77a9-c435-4a4c-9002-4316a1ab8429";
  const auto second = Ack(Handle(inspector, retry, [&] {
    ++sampled;
    return Observed(runtime_status::IdentityStatus::kVerified);
  }, kWallNow + 20s, kSteadyNow + 20s));
  CHECK(sampled == 1);
  CHECK(second.at("command_id") == retry.at("command_id"));
  CHECK(second.at("fact") == first.at("fact"));
  CHECK(second.at("observed_at") == first.at("observed_at"));
}

TEST_CASE("相同动作身份不同内容拒绝且不覆盖首次结果") {
  experiment::PiLinkInspector inspector("cns_rpi");
  const auto first = Ack(Handle(inspector, Request(), [] { return Observed(); }));
  auto changed = Request();
  changed["lease_version"] = 4;
  const auto conflict = Ack(Handle(inspector, changed, [] {
    FAIL("冲突请求不应重新采样");
    return Observed();
  }));
  CHECK(conflict.at("status") == "rejected");
  CHECK(conflict.at("error_code") == "duplicate_conflict");
  CHECK_FALSE(conflict.contains("fact"));
  const auto replay = Ack(Handle(inspector, Request(), [] {
    FAIL("原请求重投不应重新采样");
    return Observed();
  }));
  CHECK(replay == first);
}

TEST_CASE("新动作过期先拒绝不采样但旧动作过期后仍命中终态") {
  experiment::PiLinkInspector inspector("cns_rpi");
  auto expired = Request();
  expired["expires_at"] = "2026-09-22T09:00:01.000Z";
  const auto first = Ack(Handle(inspector, expired, [] {
    FAIL("已过期的新动作不得采样");
    return Observed();
  }));
  CHECK(first.at("error_code") == "action_expired");
  const auto replay = Ack(Handle(inspector, expired, [] {
    FAIL("过期动作重投不得采样");
    return Observed();
  }, kWallNow + 20s, kSteadyNow + 20s));
  CHECK(replay == first);
}

TEST_CASE("错误设备topic目标与非持久化身份不能认领实验动作") {
  experiment::PiLinkInspector inspector("cns_rpi");
  const auto sample = [] { return Observed(); };
  CHECK_FALSE(inspector.Handle("cns_rpi/OTHER/experiment/set", Request().dump(),
                               Bound(), sample, kWallNow, kSteadyNow).publication);
  auto wrong_target = Request();
  wrong_target["target"]["device_id"] = "DCDWCNS1OTHER";
  const auto ack = Ack(Handle(inspector, wrong_target, sample));
  CHECK(ack.at("error_code") == "identity_unavailable");
  CHECK_FALSE(inspector.Handle(kSetTopic, Request().dump(), std::nullopt,
                               sample, kWallNow, kSteadyNow).publication);
}

TEST_CASE("未知操作与无法采样均返回规定的拒绝错误码") {
  experiment::PiLinkInspector inspector("cns_rpi");
  auto unsupported = Request();
  unsupported["operation"] = "change_baud";
  const auto rejected = Ack(Handle(inspector, unsupported, [] {
    FAIL("未知操作不得采样");
    return Observed();
  }));
  CHECK(rejected.at("error_code") == "unsupported_operation");
  auto missing = Request();
  missing["action_id"] = "d6e627f2-4c02-49b5-a7ac-976d60858064";
  missing["request_id"] = missing["action_id"];
  const auto unavailable = Ack(Handle(inspector, missing, [] -> std::optional<experiment::Observation> {
    return std::nullopt;
  }));
  CHECK(unavailable.at("error_code") == "observation_unavailable");
}

TEST_CASE("错误字段UUID租约时间和超长载荷均不得生成猜测ACK") {
  experiment::PiLinkInspector inspector("cns_rpi");
  const auto sample = [] { return Observed(); };
  for (const auto& mutator : std::array<void (*)(nlohmann::json&), 6>{
           [](nlohmann::json& j) { j["unexpected"] = 1; },
           [](nlohmann::json& j) { j["command_id"] = "not-a-uuid"; },
           [](nlohmann::json& j) { j["lease_version"] = -1; },
           [](nlohmann::json& j) { j["request_id"] = kCommandId; },
           [](nlohmann::json& j) { j["expires_at"] = "2026-09-22 09:00:30"; },
           [](nlohmann::json& j) { j["target"]["unexpected"] = true; },
       }) {
    auto malformed = Request();
    mutator(malformed);
    CHECK_FALSE(Handle(inspector, malformed, sample).publication);
  }
  auto oversized = Request().dump() + std::string(4096, ' ');
  CHECK_FALSE(inspector.Handle(kSetTopic, oversized, Bound(), sample,
                               kWallNow, kSteadyNow).publication);
}

TEST_CASE("终态缓存容量为128且300秒内保留") {
  experiment::PiLinkInspector inspector("cns_rpi");
  for (int index = 0; index < 129; ++index) {
    auto request = Request();
    char id[37];
    std::snprintf(id, sizeof(id), "00000000-0000-4000-8000-%012x", index);
    request["action_id"] = id;
    request["request_id"] = id;
    CHECK(Handle(inspector, request, [] { return Observed(); },
                 kWallNow, kSteadyNow + std::chrono::milliseconds(index)).publication);
  }
  CHECK(inspector.CachedActionCount() == 128);
}

TEST_CASE("发布失败和断线后待发ACK保留原文且有界") {
  experiment::AckOutbox outbox;
  experiment::Publication original{.topic = kAckTopic,
                                   .payload = "{\"status\":\"completed\"}",
                                   .qos = 2, .retain = false};
  CHECK(outbox.Enqueue(original));
  CHECK_FALSE(outbox.FlushOne([](const experiment::Publication& message) {
    CHECK(message.payload == "{\"status\":\"completed\"}");
    return false;
  }));
  CHECK(outbox.Size() == 1);
  CHECK(outbox.FlushOne([&](const experiment::Publication& message) {
    CHECK(message.topic == original.topic);
    CHECK(message.payload == original.payload);
    CHECK(message.qos == 2);
    CHECK_FALSE(message.retain);
    return true;
  }));
  CHECK(outbox.Size() == 0);
  for (std::size_t index = 0; index < experiment::AckOutbox::kCapacity; ++index) {
    CHECK(outbox.Enqueue(original));
  }
  CHECK_FALSE(outbox.Enqueue(original));
  CHECK(outbox.Size() == experiment::AckOutbox::kCapacity);
}
