#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <variant>

#include "experiment/deferred_serial_request.hpp"
#include "experiment/m1_request.hpp"
#include "experiment/uart_echo_request.hpp"

namespace {
experiment::UartEchoRequest Echo(std::string action_id, std::string text = "A") {
  experiment::UartEchoRequest request;
  request.command_id = "命令1";
  request.request_id = "请求1";
  request.device_id = "DCDWCNS1GHC0G6LF8MY6";
  request.session_id = "会话1";
  request.action_id = std::move(action_id);
  request.lease_version = 1;
  request.expires_at = "2026-10-13T12:00:00.000Z";
  request.expiry = std::chrono::system_clock::time_point{std::chrono::seconds{1791892800}};
  request.text_bytes.assign(text.begin(), text.end());
  return request;
}

experiment::M1Request Probe(std::string action_id) {
  experiment::M1Request request;
  request.command_id = "命令2";
  request.request_id = "请求2";
  request.device_id = "DCDWCNS1GHC0G6LF8MY6";
  request.session_id = "会话1";
  request.action_id = std::move(action_id);
  request.lease_version = 1;
  request.operation = experiment::M1Operation::kUartProbe;
  request.expires_at = "2026-10-13T12:00:00.000Z";
  request.expiry = std::chrono::system_clock::time_point{std::chrono::seconds{1791892800}};
  return request;
}
}  // namespace

TEST_CASE("待复核实验动作只移交一次且原有效期不被重写") {
  experiment::DeferredSerialRequestQueue queue;
  const auto request = Echo("动作1");
  CHECK(queue.Enqueue(request) == experiment::DeferResult::kQueued);
  REQUIRE(queue.Front() != nullptr);
  CHECK(std::get<experiment::UartEchoRequest>(*queue.Front()).expiry == request.expiry);
  const auto taken = queue.Take();
  REQUIRE(taken.has_value());
  CHECK(std::get<experiment::UartEchoRequest>(*taken).action_id == "动作1");
  CHECK_FALSE(queue.Take().has_value());
}

TEST_CASE("重复动作不再次入队且同ID不同内容拒绝") {
  experiment::DeferredSerialRequestQueue queue;
  CHECK(queue.Enqueue(Echo("动作1")) == experiment::DeferResult::kQueued);
  auto repeated = Echo("动作1");
  repeated.command_id = "命令重试";
  CHECK(queue.Enqueue(repeated) == experiment::DeferResult::kDuplicate);
  CHECK(queue.Enqueue(Echo("动作1", "B")) == experiment::DeferResult::kConflict);
  REQUIRE(queue.Front() != nullptr);
  CHECK(std::get<experiment::UartEchoRequest>(*queue.Front()).text_bytes ==
        std::vector<std::uint8_t>{'A'});
}

TEST_CASE("身份已复核但旧动作仍占串口时槽内重投继续由待发槽拦截") {
  experiment::DeferredSerialRequestQueue queue;
  CHECK_FALSE(queue.ShouldIntercept(false));
  CHECK(queue.ShouldIntercept(true));
  REQUIRE(queue.Enqueue(Echo("动作1")) == experiment::DeferResult::kQueued);
  CHECK(queue.ShouldIntercept(false));
  CHECK(queue.Enqueue(Echo("动作1")) == experiment::DeferResult::kDuplicate);
  REQUIRE(queue.Take().has_value());
  CHECK_FALSE(queue.ShouldIntercept(false));
}

TEST_CASE("待发槽中的H1动作ID不能被本地M1换速动作抢占") {
  experiment::DeferredSerialRequestQueue queue;
  CHECK_FALSE(queue.ShouldInterceptM1(true, true));
  REQUIRE(queue.Enqueue(Echo("动作1")) == experiment::DeferResult::kQueued);
  CHECK(queue.ShouldInterceptM1(true, true));
  CHECK(queue.ShouldInterceptM1(false, true));
  auto same_id_local = Probe("动作1");
  same_id_local.operation = experiment::M1Operation::kSetPiBaud;
  same_id_local.baud_rate = 57600;
  CHECK(queue.Enqueue(same_id_local) == experiment::DeferResult::kConflict);
  auto another_local = same_id_local;
  another_local.action_id = "动作2";
  CHECK(queue.Enqueue(another_local) == experiment::DeferResult::kFull);
  REQUIRE(queue.Front() != nullptr);
  CHECK(std::get<experiment::UartEchoRequest>(*queue.Front()).action_id == "动作1");
}

TEST_CASE("候选身份冲突后新串口实验动作立即拒绝而非入槽") {
  experiment::DeferredSerialRequestQueue queue;
  CHECK(queue.Decide(true, true, false) ==
        experiment::SerialRequestDecision::kRejectIdentity);
  CHECK_FALSE(queue.Front());
  CHECK(queue.Decide(true, true, true) ==
        experiment::SerialRequestDecision::kProceed);
  CHECK(queue.Decide(true, false, false) ==
        experiment::SerialRequestDecision::kDefer);
}

TEST_CASE("单槽被占用时不同动作不能挤掉原动作") {
  experiment::DeferredSerialRequestQueue queue;
  CHECK(queue.Enqueue(Echo("动作1")) == experiment::DeferResult::kQueued);
  CHECK(queue.Enqueue(Probe("动作2")) == experiment::DeferResult::kFull);
  REQUIRE(queue.Front() != nullptr);
  CHECK(std::get<experiment::UartEchoRequest>(*queue.Front()).action_id == "动作1");
}

TEST_CASE("待发槽拒绝沿用实验终态字段与设备ACK主题") {
  const auto publication = experiment::BuildDeferredRejection(
      Echo("动作1"), "cns", "duplicate_conflict",
      std::chrono::system_clock::time_point{std::chrono::seconds{1000}});
  CHECK(publication.topic == "cns/DCDWCNS1GHC0G6LF8MY6/experiment/ack");
  CHECK(publication.qos == 2);
  CHECK_FALSE(publication.retain);
  const auto body = nlohmann::json::parse(publication.payload);
  CHECK(body.at("schema_version") == 1);
  CHECK(body.at("command_id") == "命令1");
  CHECK(body.at("action_id") == "动作1");
  CHECK(body.at("operation") == "uart_echo");
  CHECK(body.at("status") == "rejected");
  CHECK(body.at("error_code") == "duplicate_conflict");
  CHECK(body.at("observed_at") == "1970-01-01T00:16:40.000Z");
}

TEST_CASE("待发H1与M1请求还原后仍通过原始严格解析") {
  auto echo = Echo("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa", "中文Echo");
  echo.command_id = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
  echo.request_id = echo.action_id;
  echo.session_id = "cccccccc-cccc-cccc-cccc-cccccccccccc";
  const auto echo_payload = experiment::SerializeDeferredRequest(echo);
  const auto parsed_echo = experiment::ParseUartEchoRequest(
      echo_payload, echo.device_id, std::chrono::system_clock::now());
  REQUIRE(parsed_echo.has_value());
  CHECK(parsed_echo->text_bytes == echo.text_bytes);
  CHECK(parsed_echo->expiry == echo.expiry);

  auto m1 = Probe("dddddddd-dddd-dddd-dddd-dddddddddddd");
  m1.command_id = "eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee";
  m1.request_id = m1.action_id;
  m1.session_id = echo.session_id;
  m1.operation = experiment::M1Operation::kSetF407Baud;
  m1.baud_rate = 57600;
  const auto m1_payload = experiment::SerializeDeferredRequest(m1);
  const auto parsed_m1 = experiment::ParseM1Request(m1_payload, m1.device_id);
  REQUIRE(parsed_m1.has_value());
  CHECK(parsed_m1->operation == experiment::M1Operation::kSetF407Baud);
  CHECK(parsed_m1->baud_rate == 57600);
  CHECK(parsed_m1->expiry == m1.expiry);
}

TEST_CASE("待复核未完成时不移交有效请求但已过期请求交原事务处理") {
  const auto request = Echo("动作1");
  const auto before = request.expiry - std::chrono::milliseconds(1);
  CHECK_FALSE(experiment::ShouldReplayDeferred(request, before, false, true, true));
  CHECK_FALSE(experiment::ShouldReplayDeferred(request, before, true, false, true));
  CHECK_FALSE(experiment::ShouldReplayDeferred(request, before, true, true, false));
  CHECK(experiment::ShouldReplayDeferred(request, before, true, true, true));
  CHECK(experiment::ShouldReplayDeferred(request, request.expiry, false, false, false));
}
