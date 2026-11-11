#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include "experiment/gnss_request.hpp"

using Json = nlohmann::json;
namespace {
Json Request(std::string operation = "gnss_capture_start") {
  Json j{{"schema_version",1},{"command_id","00000000-0000-4000-8000-000000000001"},
    {"request_id","00000000-0000-4000-8000-000000000003"},
    {"session_id","00000000-0000-4000-8000-000000000002"},
    {"action_id","00000000-0000-4000-8000-000000000003"},
    {"target",{{"device_id","DCDWCNS1GNSS0000001"}}},{"lease_version",1},
    {"operation",operation},{"expires_at","2099-01-01T00:00:00.000Z"}};
  if (operation != "gnss_status") j["parameters"] = {{"capture_id","00000000-0000-4000-8000-000000000004"}};
  if (operation == "gnss_capture_start") j["parameters"]["duration_seconds"] = 300;
  return j;
}
}

TEST_CASE("GNSS request preserves typed capture and action correlation") {
  auto j = Request();
  auto r = experiment::ParseGnssRequest(j.dump());
  REQUIRE(r);
  CHECK(r->duration_seconds == 300);
  CHECK(r->capture_id == "00000000-0000-4000-8000-000000000004");
  CHECK(r->device_id == "DCDWCNS1GNSS0000001");
  const auto comparison = r->comparison;
  j["command_id"] = "00000000-0000-4000-8000-000000000005";
  r = experiment::ParseGnssRequest(j.dump());
  REQUIRE(r);
  CHECK(r->comparison == comparison);
  CHECK(experiment::ParseGnssRequest(Request("gnss_capture_stop").dump()));
  CHECK(experiment::ParseGnssRequest(Request("gnss_status").dump()));
}

TEST_CASE("GNSS rejects unusable capture parameters at the external boundary") {
  auto j = Request();
  SUBCASE("zero duration") { j["parameters"]["duration_seconds"] = 0; }
  SUBCASE("long duration") { j["parameters"]["duration_seconds"] = 1801; }
  SUBCASE("noninteger duration") { j["parameters"]["duration_seconds"] = 1.5; }
  SUBCASE("negative lease") { j["lease_version"] = -1; }
  SUBCASE("path instead of capture UUID") { j["parameters"]["capture_id"] = "../../archive"; }
  SUBCASE("extra parameter") { j["parameters"]["output_directory"] = "/tmp/other"; }
  SUBCASE("bad calendar date") { j["expires_at"] = "2099-02-30T00:00:00.000Z"; }
  SUBCASE("wrong request correlation") { j["request_id"] = "00000000-0000-4000-8000-000000000008"; }
  SUBCASE("extra top field") { j["enabled"] = true; }
  SUBCASE("wrong target") { j["target"]["device_id"] = "arbitrary device"; }
  CHECK_FALSE(experiment::ParseGnssRequest(j.dump()));
}

TEST_CASE("GNSS operation routing does not claim other experiments") {
  CHECK(experiment::IsGnssOperation(Request().dump()));
  CHECK_FALSE(experiment::IsGnssOperation("{}"));
  auto j = Request(); j["operation"] = "rtk_base_status";
  CHECK_FALSE(experiment::IsGnssOperation(j.dump()));
  CHECK_FALSE(experiment::ParseGnssRequest(std::string(4097,' ')));
  j = Request("gnss_status"); j["parameters"] = Json::object();
  CHECK_FALSE(experiment::ParseGnssRequest(j.dump()));
}
