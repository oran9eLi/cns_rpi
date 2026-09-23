#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "experiment/uart_echo_request.hpp"

namespace {

using namespace std::chrono_literals;
constexpr auto kNow = std::chrono::sys_days{std::chrono::year{2026}/9/23} + 9h;
constexpr const char* kDeviceId = "DCDWCNS1S2MEAG1VTA0C";

nlohmann::json Request(std::string text = "A中") {
  return {{"schema_version", 1},
          {"command_id", "7d15e276-c7e1-4f52-aaee-7314a6417667"},
          {"request_id", "f886f45b-01f8-4a54-b2b6-a65b07477aa1"},
          {"target", {{"device_id", kDeviceId}}},
          {"session_id", "c4191037-9e2a-480c-a995-2174e4672563"},
          {"action_id", "f886f45b-01f8-4a54-b2b6-a65b07477aa1"},
          {"lease_version", 3},
          {"operation", "uart_echo"},
          {"text", std::move(text)},
          {"expires_at", "2026-09-23T09:00:30.000Z"}};
}

std::expected<experiment::UartEchoRequest, std::string> Parse(
    const nlohmann::json& request) {
  return experiment::ParseUartEchoRequest(request.dump(), kDeviceId, kNow);
}

}  // namespace

TEST_CASE("受限中英混合文本原样保留UTF8字节") {
  const auto parsed = Parse(Request());
  REQUIRE(parsed.has_value());
  CHECK(parsed->text_bytes == std::vector<std::uint8_t>{0x41, 0xE4, 0xB8, 0xAD});
  CHECK(parsed->action_id == parsed->request_id);
  CHECK(parsed->lease_version == 3);
}

TEST_CASE("文本长度和空白控制字符越界时拒绝") {
  CHECK_FALSE(Parse(Request("")).has_value());
  CHECK_FALSE(Parse(Request(std::string(49, 'A'))).has_value());
  std::string seventeen_chinese;
  for (int index = 0; index < 17; ++index) seventeen_chinese += "中";
  CHECK_FALSE(Parse(Request(seventeen_chinese)).has_value());
  std::string sixteen_chinese;
  for (int index = 0; index < 16; ++index) sixteen_chinese += "中";
  CHECK(Parse(Request(sixteen_chinese)).has_value());
  CHECK_FALSE(Parse(Request("　")).has_value());
  CHECK_FALSE(Parse(Request("\t")).has_value());
  CHECK_FALSE(Parse(Request("A\nB")).has_value());
  CHECK_FALSE(Parse(Request(std::string("A\xC2\x85"))).has_value());
  CHECK(Parse(Request(" A ")).has_value());
}

TEST_CASE("不接受畸形UTF8或额外字段") {
  std::string raw = Request("A").dump();
  const auto location = raw.find("\"text\":\"A\"");
  REQUIRE(location != std::string::npos);
  raw.replace(location, 10, std::string("\"text\":\"A\xC0\xAF\"", 12));
  CHECK_FALSE(experiment::ParseUartEchoRequest(raw, kDeviceId, kNow).has_value());
  auto request = Request();
  request["raw_bytes"] = "00FF";
  CHECK_FALSE(Parse(request).has_value());
}

TEST_CASE("错误目标动作身份和过期请求被拒绝") {
  auto request = Request();
  request["target"]["device_id"] = "DCDWCNS1OTHERDEVICE";
  CHECK_FALSE(Parse(request).has_value());
  request = Request();
  request["request_id"] = "00000000-0000-0000-0000-000000000000";
  CHECK_FALSE(Parse(request).has_value());
  request = Request();
  request["expires_at"] = "2026-09-23T08:59:59.000Z";
  CHECK_FALSE(Parse(request).has_value());
  request = Request();
  request["lease_version"] = -1;
  CHECK_FALSE(Parse(request).has_value());
}
