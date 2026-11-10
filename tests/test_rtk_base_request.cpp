#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "experiment/rtk_base_request.hpp"

namespace {
using Json = nlohmann::json;

Json Request(std::string operation = "rtk_base_survey_in") {
  Json result = {{"schema_version", 1},
      {"command_id", "7d15e276-c7e1-4f52-aaee-7314a6417667"},
      {"request_id", "f886f45b-01f8-4a54-b2b6-a65b07477aa1"},
      {"action_id", "f886f45b-01f8-4a54-b2b6-a65b07477aa1"},
      {"session_id", "c4191037-9e2a-480c-a995-2174e4672563"},
      {"target", {{"device_id", "PX4-1234567890ABCDEF"}}},
      {"lease_version", 3}, {"operation", operation},
      {"expires_at", "2026-11-06T12:00:30.123Z"}};
  if (operation == "rtk_base_survey_in") {
    result["parameters"] = {{"minimum_duration_s", 60}, {"accuracy_limit_m", 2.0}};
  }
  return result;
}
}  // namespace

TEST_CASE("RTK路由识别基站操作和移动站实验操作") {
  CHECK(experiment::IsRtkBaseOperation(Request().dump()));
  CHECK(experiment::IsRtkBaseOperation(Request("rtk_base_status").dump()));
  CHECK_FALSE(experiment::IsRtkBaseOperation(Request("video_status").dump()));
  CHECK_FALSE(experiment::IsRtkBaseOperation(Request("inspect_pi_link").dump()));
  CHECK_FALSE(experiment::IsRtkBaseOperation("not-json"));
  CHECK_FALSE(experiment::IsRtkBaseOperation("[]"));
  CHECK_FALSE(experiment::IsRtkBaseOperation(std::string(4097, ' ')));
}

TEST_CASE("移动站操作复用严格信封且不能传入任意串口数据") {
  for (const auto* operation : {"rtk_rover_start", "rtk_rover_pause", "rtk_rover_resume",
                                "rtk_rover_stop", "rtk_rover_status"}) {
    auto request = Request(operation);
    CHECK(experiment::IsRtkBaseOperation(request.dump()));
    const auto parsed = experiment::ParseRtkBaseRequest(request.dump());
    REQUIRE(parsed);
    CHECK(parsed->operation == operation);
    request["parameters"] = {{"raw_hex", "d30000"}};
    CHECK_FALSE(experiment::ParseRtkBaseRequest(request.dump()));
  }
}

TEST_CASE("RTK测量请求保留显式参数和外层关联字段") {
  const auto parsed = experiment::ParseRtkBaseRequest(Request().dump());
  REQUIRE(parsed);
  CHECK(parsed->command_id == "7d15e276-c7e1-4f52-aaee-7314a6417667");
  CHECK(parsed->device_id == "PX4-1234567890ABCDEF");
  CHECK(parsed->session_id == "c4191037-9e2a-480c-a995-2174e4672563");
  CHECK(parsed->action_id == "f886f45b-01f8-4a54-b2b6-a65b07477aa1");
  CHECK(parsed->operation == "rtk_base_survey_in");
  CHECK(parsed->lease_version == 3);
  CHECK(parsed->minimum_duration_s == 60);
  CHECK(parsed->accuracy_limit_m == 2.0);
  CHECK(parsed->expiry == std::chrono::sys_days{std::chrono::year{2026}/11/6} +
      std::chrono::hours{12} + std::chrono::seconds{30} + std::chrono::milliseconds{123});
}

TEST_CASE("RTK状态请求不携带测量参数") {
  auto request = Request("rtk_base_status");
  const auto parsed = experiment::ParseRtkBaseRequest(request.dump());
  REQUIRE(parsed);
  CHECK(parsed->operation == "rtk_base_status");
  CHECK(parsed->minimum_duration_s == 0);
  CHECK(parsed->accuracy_limit_m == 0.0);
  request["parameters"] = Json::object();
  CHECK_FALSE(experiment::ParseRtkBaseRequest(request.dump()));
}

TEST_CASE("RTK测量参数在入口校验必需字段类型和范围") {
  for (const auto& [key, value] : std::vector<std::pair<std::string, Json>>{
           {"minimum_duration_s", 9}, {"minimum_duration_s", 86401},
           {"minimum_duration_s", -1}, {"minimum_duration_s", 60.0},
           {"minimum_duration_s", "60"}, {"minimum_duration_s", 4294967356ULL},
           {"accuracy_limit_m", 0.09}, {"accuracy_limit_m", 100.1},
           {"accuracy_limit_m", "2.0"}, {"accuracy_limit_m", nullptr}}) {
    auto request = Request();
    request["parameters"][key] = value;
    CHECK_FALSE(experiment::ParseRtkBaseRequest(request.dump()));
  }
  for (const auto* key : {"minimum_duration_s", "accuracy_limit_m"}) {
    auto request = Request();
    request["parameters"].erase(key);
    CHECK_FALSE(experiment::ParseRtkBaseRequest(request.dump()));
  }
  auto request = Request();
  request.erase("parameters");
  CHECK_FALSE(experiment::ParseRtkBaseRequest(request.dump()));
  request = Request();
  request["parameters"]["device"] = "/dev/ttyACM0";
  CHECK_FALSE(experiment::ParseRtkBaseRequest(request.dump()));
}

TEST_CASE("RTK测量参数接受两端边界和整数精度门限") {
  for (const auto& [duration, accuracy] :
       std::vector<std::pair<int, double>>{{10, 0.1}, {86400, 100.0}}) {
    auto request = Request();
    request["parameters"] = {{"minimum_duration_s", duration}, {"accuracy_limit_m", accuracy}};
    const auto parsed = experiment::ParseRtkBaseRequest(request.dump());
    REQUIRE(parsed);
    CHECK(parsed->minimum_duration_s == duration);
    CHECK(parsed->accuracy_limit_m == accuracy);
  }
  auto request = Request();
  request["parameters"]["accuracy_limit_m"] = 2;
  CHECK(experiment::ParseRtkBaseRequest(request.dump()).has_value());
}

TEST_CASE("RTK沿用严格的实验外层身份租约和UTC约束") {
  for (const auto& [field, value] : std::vector<std::pair<std::string, Json>>{
           {"schema_version", 2}, {"schema_version", 1.0}, {"lease_version", -1},
           {"lease_version", 0}, {"lease_version", 3.0}, {"command_id", "not-uuid"},
           {"action_id", "not-uuid"}, {"session_id", "not-uuid"},
           {"request_id", "7d15e276-c7e1-4f52-aaee-7314a6417667"},
           {"expires_at", "2026-02-30T12:00:30.000Z"},
           {"expires_at", "2026-11-06T12:00:30Z"},
           {"expires_at", "2026-11-06T24:00:30.000Z"},
           {"expires_at", "2026-11-06T12:60:30.000Z"},
           {"expires_at", "2026-11-06T12:00:60.000Z"},
           {"operation", "rtk_base_reset"}, {"unknown", true}}) {
    auto request = Request();
    request[field] = value;
    CHECK_FALSE(experiment::ParseRtkBaseRequest(request.dump()));
  }
  auto request = Request();
  request["target"]["device_id"] = "PX4/+";
  CHECK_FALSE(experiment::ParseRtkBaseRequest(request.dump()));
  request = Request();
  request["target"]["other"] = "anything";
  CHECK_FALSE(experiment::ParseRtkBaseRequest(request.dump()));
  for (const auto* key : {"schema_version", "command_id", "request_id", "target",
                        "session_id", "action_id", "lease_version", "expires_at"}) {
    auto incomplete = Request();
    incomplete.erase(key);
    CHECK_FALSE(experiment::ParseRtkBaseRequest(incomplete.dump()));
  }
  CHECK_FALSE(experiment::ParseRtkBaseRequest("not-json"));
  CHECK_FALSE(experiment::ParseRtkBaseRequest("[]"));
  CHECK_FALSE(experiment::ParseRtkBaseRequest(std::string(4097, ' ')));
}

TEST_CASE("RTK动作比较只忽略command_id且保留参数变化") {
  auto first = Request();
  auto retry = first;
  retry["command_id"] = "f347f0c2-1f3e-4fb2-bc0a-00dff10b0001";
  const auto a = experiment::ParseRtkBaseRequest(first.dump());
  const auto b = experiment::ParseRtkBaseRequest(retry.dump());
  REQUIRE(a);
  REQUIRE(b);
  CHECK(a->comparison == b->comparison);
  retry["parameters"]["accuracy_limit_m"] = 1.0;
  const auto changed = experiment::ParseRtkBaseRequest(retry.dump());
  REQUIRE(changed);
  CHECK(a->comparison != changed->comparison);
}
