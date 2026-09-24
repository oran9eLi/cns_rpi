#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <limits>
#include <nlohmann/json.hpp>

#include "experiment/m1_request.hpp"

namespace {
constexpr const char* kDeviceId = "DCDWCNS1S2MEAG1VTA0C";

nlohmann::json Request(std::string operation = "set_f407_baud") {
  nlohmann::json value{
      {"schema_version", 1},
      {"command_id", "7d15e276-c7e1-4f52-aaee-7314a6417667"},
      {"request_id", "f886f45b-01f8-4a54-b2b6-a65b07477aa1"},
      {"target", {{"device_id", kDeviceId}}},
      {"session_id", "c4191037-9e2a-480c-a995-2174e4672563"},
      {"action_id", "f886f45b-01f8-4a54-b2b6-a65b07477aa1"},
      {"lease_version", 3},
      {"operation", operation},
      {"expires_at", "2026-09-24T09:00:30.000Z"},
  };
  if (operation != "uart_probe") value["baud_rate"] = 57600;
  return value;
}

auto Parse(const nlohmann::json& value) {
  return experiment::ParseM1Request(value.dump(), kDeviceId);
}
}  // namespace

TEST_CASE("三种M1操作保留动作身份和受限波特率") {
  const auto f407 = Parse(Request());
  REQUIRE(f407.has_value());
  CHECK(f407->operation == experiment::M1Operation::kSetF407Baud);
  CHECK(f407->baud_rate == 57600);
  CHECK(f407->action_id == f407->request_id);
  CHECK(f407->lease_version == 3);

  auto pi_request = Request("set_pi_baud");
  pi_request["baud_rate"] = 115200;
  const auto pi = Parse(pi_request);
  REQUIRE(pi.has_value());
  CHECK(pi->operation == experiment::M1Operation::kSetPiBaud);
  CHECK(pi->baud_rate == 115200);

  const auto probe = Parse(Request("uart_probe"));
  REQUIRE(probe.has_value());
  CHECK(probe->operation == experiment::M1Operation::kUartProbe);
  CHECK_FALSE(probe->baud_rate.has_value());
}

TEST_CASE("M1请求拒绝额外字段和错误目标，合法结构的超范围速率交事务层回执") {
  auto value = Request(); value["raw_frame_hex"] = "FD00";
  CHECK_FALSE(Parse(value).has_value());
  value = Request(); value["target"]["device_id"] = "DCDWCNS1OTHERDEVICE";
  CHECK_FALSE(Parse(value).has_value());
  value = Request(); value["baud_rate"] = 230400;
  const auto oversized_baud = Parse(value);
  INFO((oversized_baud.has_value() ? std::string{} : oversized_baud.error()));
  REQUIRE(oversized_baud.has_value());
  value = Request(); value["baud_rate"] = 4295024896ULL;
  CHECK_FALSE(Parse(value).has_value());
  value = Request(); value["baud_rate"] = 115200;
  REQUIRE(Parse(value).has_value());
  value = Request("uart_probe"); value["baud_rate"] = 57600;
  CHECK_FALSE(Parse(value).has_value());
}

TEST_CASE("M1请求拒绝身份、时间和大小越界") {
  auto value = Request(); value["request_id"] = "00000000-0000-0000-0000-000000000000";
  CHECK_FALSE(Parse(value).has_value());
  value = Request(); value["lease_version"] = 0;
  CHECK_FALSE(Parse(value).has_value());
  value = Request(); value["lease_version"] = std::numeric_limits<std::uint64_t>::max();
  const auto high_lease = Parse(value);
  REQUIRE(high_lease.has_value());
  CHECK(high_lease->lease_version == std::numeric_limits<std::uint64_t>::max());
  value = Request(); value["expires_at"] = "2026-02-30T09:00:30.000Z";
  CHECK_FALSE(Parse(value).has_value());
  value = Request(); value["padding"] = std::string(4096, 'A');
  CHECK_FALSE(Parse(value).has_value());
  CHECK_FALSE(experiment::ParseM1Request("{不完整", kDeviceId).has_value());
}

TEST_CASE("实验分流仅识别三种明确M1操作") {
  CHECK(experiment::IsM1Operation(Request().dump()));
  CHECK(experiment::IsM1Operation(Request("set_pi_baud").dump()));
  CHECK(experiment::IsM1Operation(Request("uart_probe").dump()));
  CHECK_FALSE(experiment::IsM1Operation("{不完整"));
  CHECK_FALSE(experiment::IsM1Operation(R"({"operation":"uart_echo"})"));
}
