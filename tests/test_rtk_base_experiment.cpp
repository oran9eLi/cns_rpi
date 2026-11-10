#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "experiment/rtk_base_experiment.hpp"

using Json = nlohmann::json;
using Wall = std::chrono::system_clock;

namespace {
Json Request(std::string operation = "rtk_base_status") {
  Json value{{"schema_version", 1},
      {"command_id", "00000000-0000-4000-8000-000000000001"},
      {"request_id", "00000000-0000-4000-8000-000000000003"},
      {"session_id", "00000000-0000-4000-8000-000000000002"},
      {"action_id", "00000000-0000-4000-8000-000000000003"},
      {"target", {{"device_id", "DCDWCNS1RTKTEST00001"}}}, {"lease_version", 1},
      {"operation", operation}, {"expires_at", "2099-01-01T00:00:00.000Z"}};
  if (operation == "rtk_base_survey_in")
    value["parameters"] = {{"minimum_duration_s", 60}, {"accuracy_limit_m", 2.0}};
  return value;
}
constexpr auto kTopic = "cns_rpi/DCDWCNS1RTKTEST00001/experiment/set";
}

TEST_CASE("rejected authorized resume remains terminal after pause and renews its lease") {
  experiment::RtkBaseExperiment controller("cns_rpi");
  const auto now = Wall::now();
  std::string mode = "running";
  const auto sample = [&] { return Json{{"relay", {{"state", mode}}}}; };
  const auto start = [](auto, auto) -> std::expected<void, std::string> { return {}; };
  const auto operate = [&](std::string_view operation) -> std::expected<void, std::string> {
    if (operation == "rtk_rover_resume") {
      if (mode != "paused") return std::unexpected("not paused");
      mode = "running";
    } else if (operation == "rtk_rover_pause") mode = "paused";
    return {};
  };
  auto rejected_request = Request("rtk_rover_resume");
  rejected_request["lease_version"] = 2;
  const auto first = controller.Handle(kTopic, rejected_request.dump(), "DCDWCNS1RTKTEST00001",
      sample, start, now, operate, sample);
  REQUIRE(first.publication);
  const auto rejected = Json::parse(first.publication->payload);
  CHECK(rejected["error_code"] == "rtk_rover_rejected");
  auto pause = Request("rtk_rover_pause");
  pause["action_id"] = pause["request_id"] = "00000000-0000-4000-8000-000000000004";
  auto outcome = controller.Handle(kTopic, pause.dump(), "DCDWCNS1RTKTEST00001",
      sample, start, now, operate, sample);
  REQUIRE(outcome.publication);
  CHECK(Json::parse(outcome.publication->payload)["error_code"] == "stale_lease");
  CHECK(mode == "running");
  pause["lease_version"] = 2;
  outcome = controller.Handle(kTopic, pause.dump(), "DCDWCNS1RTKTEST00001",
      sample, start, now, operate, sample);
  REQUIRE(outcome.publication);
  REQUIRE(mode == "paused");
  outcome = controller.Handle(kTopic, rejected_request.dump(), "DCDWCNS1RTKTEST00001",
      sample, start, now + std::chrono::seconds(1), operate, sample);
  REQUIRE(outcome.publication);
  CHECK(Json::parse(outcome.publication->payload) == rejected);
  CHECK(mode == "paused");
}

TEST_CASE("rover status belongs to same controller and never starts survey") {
  experiment::RtkBaseExperiment controller("cns_rpi");
  const auto result = controller.Handle(kTopic, Request("rtk_rover_status").dump(),
      "DCDWCNS1RTKTEST00001", [] { return Json::object(); },
      [](auto, auto) -> std::expected<void, std::string> { FAIL("rover query configured base"); return {}; },
      Wall::now());
  REQUIRE(result.publication);
  CHECK(Json::parse(result.publication->payload)["error_code"] == "rtk_rover_unavailable");
}

TEST_CASE("rover actions share base lease and dedup and return actual RTK fact") {
  experiment::RtkBaseExperiment controller("cns_rpi");
  const auto base = [] { return Json{{"connected", true}}; };
  const auto start = [](auto, auto) -> std::expected<void, std::string> {
    FAIL("rover action invoked survey"); return {};
  };
  Json state{{"relay", {{"state", "stopped"}}}};
  int calls = 0;
  const auto operate = [&](std::string_view operation) -> std::expected<void, std::string> {
    CHECK(operation == "rtk_rover_start");
    ++calls; state["relay"]["state"] = "waiting_base"; return {};
  };
  const auto sample = [&] { return state; };
  auto request = Request("rtk_rover_start");
  request["lease_version"] = 2;
  auto result = controller.Handle(kTopic, request.dump(), "DCDWCNS1RTKTEST00001",
      base, start, Wall::now(), operate, sample);
  REQUIRE(result.publication);
  auto ack = Json::parse(result.publication->payload);
  CHECK(ack["status"] == "completed");
  CHECK(ack["fact"]["rtk"]["relay"]["state"] == "waiting_base");
  CHECK_FALSE(ack["fact"].contains("base"));
  request["command_id"] = "00000000-0000-4000-8000-000000000004";
  result = controller.Handle(kTopic, request.dump(), "DCDWCNS1RTKTEST00001",
      base, start, Wall::now(), operate, sample);
  REQUIRE(result.publication);
  CHECK(calls == 1);
  CHECK(Json::parse(result.publication->payload)["command_id"] == request["command_id"]);
  request["operation"] = "rtk_rover_stop";
  result = controller.Handle(kTopic, request.dump(), "DCDWCNS1RTKTEST00001",
      base, start, Wall::now(), operate, sample);
  REQUIRE(result.publication);
  CHECK(Json::parse(result.publication->payload)["error_code"] == "action_conflict");
  request = Request();
  request["request_id"] = request["action_id"] = "00000000-0000-4000-8000-000000000005";
  result = controller.Handle(kTopic, request.dump(), "DCDWCNS1RTKTEST00001",
      base, start, Wall::now(), operate, sample);
  REQUIRE(result.publication);
  CHECK(Json::parse(result.publication->payload)["error_code"] == "stale_lease");
}

TEST_CASE("rover status does not operate and expired start cannot turn relay on") {
  experiment::RtkBaseExperiment controller("cns_rpi");
  const auto sample = [] { return Json{{"relay", {{"state", "stopped"}}}}; };
  const auto start = [](auto, auto) -> std::expected<void, std::string> { return {}; };
  const auto operate = [](std::string_view) -> std::expected<void, std::string> {
    FAIL("query or expired action reached relay"); return {};
  };
  auto request = Request("rtk_rover_status");
  auto result = controller.Handle(kTopic, request.dump(), "DCDWCNS1RTKTEST00001",
      sample, start, Wall::now(), operate, sample);
  REQUIRE(result.publication);
  CHECK(Json::parse(result.publication->payload)["fact"]["rtk"]["relay"]["state"] == "stopped");
  request = Request("rtk_rover_start");
  request["action_id"] = request["request_id"] = "00000000-0000-4000-8000-000000000005";
  request["expires_at"] = "2020-01-01T00:00:00.000Z";
  result = controller.Handle(kTopic, request.dump(), "DCDWCNS1RTKTEST00001",
      sample, start, Wall::now(), operate, sample);
  REQUIRE(result.publication);
  CHECK(Json::parse(result.publication->payload)["error_code"] == "expired");
}

TEST_CASE("status returns live base observations under the bound CNS box identity") {
  experiment::RtkBaseExperiment controller("cns_rpi");
  const auto sample = [] { return Json{{"connected", true}, {"survey_in", {{"valid", false}}}}; };
  auto result = controller.Handle(kTopic, Request().dump(), "DCDWCNS1RTKTEST00001", sample,
      [](auto, auto) -> std::expected<void, std::string> { FAIL("status must not configure the receiver"); return {}; }, Wall::now());
  REQUIRE(result.publication);
  const auto ack = Json::parse(result.publication->payload);
  CHECK(ack["status"] == "completed");
  CHECK(ack["fact"]["base"]["survey_in"]["valid"] == false);
  CHECK(result.publication->topic == "cns_rpi/DCDWCNS1RTKTEST00001/experiment/ack");
}

TEST_CASE("survey is accepted until actual readback and duplicate actions do not restart it") {
  experiment::RtkBaseExperiment controller("cns_rpi");
  Json state{{"connected", true}, {"configuration", {{"state", "awaiting_ack"}}}};
  int starts = 0;
  const auto start = [&](std::uint32_t duration, double accuracy) -> std::expected<void, std::string> {
    CHECK(duration == 60); CHECK(accuracy == 2.0); ++starts; return {};
  };
  const auto sample = [&] { return state; };
  const auto now = Wall::now();
  const auto request = Request("rtk_base_survey_in");
  const auto first = controller.Handle(kTopic, request.dump(), "DCDWCNS1RTKTEST00001", sample, start, now);
  REQUIRE(first.publication);
  CHECK(Json::parse(first.publication->payload)["status"] == "accepted");
  CHECK_FALSE(controller.Tick(sample, [] {}, now));
  controller.Handle(kTopic, request.dump(), "DCDWCNS1RTKTEST00001", sample, start, now);
  CHECK(starts == 1);
  state["configuration"]["state"] = "verified";
  const auto completed = controller.Tick(sample, [] {}, now);
  REQUIRE(completed);
  CHECK(Json::parse(completed->payload)["status"] == "completed");
  CHECK_FALSE(controller.Tick(sample, [] {}, now));
}

TEST_CASE("wrong targets and expired requests cannot modify the receiver") {
  experiment::RtkBaseExperiment controller("cns_rpi");
  const auto sample = [] { return Json::object(); };
  const auto forbidden = [](auto, auto) -> std::expected<void, std::string> { FAIL("invalid request reached receiver"); return {}; };
  auto request = Request("rtk_base_survey_in");
  request["target"]["device_id"] = "OTHERBOX";
  auto outcome = controller.Handle(kTopic, request.dump(), "DCDWCNS1RTKTEST00001", sample, forbidden, Wall::now());
  REQUIRE(outcome.publication);
  CHECK(Json::parse(outcome.publication->payload)["error_code"] == "wrong_target");
  request = Request("rtk_base_survey_in");
  request["expires_at"] = "2020-01-01T00:00:00.000Z";
  outcome = controller.Handle(kTopic, request.dump(), "DCDWCNS1RTKTEST00001", sample, forbidden, Wall::now());
  REQUIRE(outcome.publication);
  CHECK(Json::parse(outcome.publication->payload)["error_code"] == "expired");
}

TEST_CASE("configuration failure is reported and stale leases cannot restart survey") {
  experiment::RtkBaseExperiment controller("cns_rpi");
  Json state{{"connected", true}, {"configuration", {{"state", "pending"}}}};
  const auto sample = [&] { return state; };
  const auto start = [](auto, auto) -> std::expected<void, std::string> { return {}; };
  const auto now = Wall::now();
  auto request = Request("rtk_base_survey_in");
  request["lease_version"] = 2;
  controller.Handle(kTopic, request.dump(), "DCDWCNS1RTKTEST00001", sample, start, now);
  state["configuration"]["state"] = "failed";
  auto completed = controller.Tick(sample, [] {}, now);
  REQUIRE(completed);
  CHECK(Json::parse(completed->payload)["status"] == "failed");
  request["lease_version"] = 1;
  request["action_id"] = request["request_id"] = "00000000-0000-4000-8000-000000000004";
  const auto outcome = controller.Handle(kTopic, request.dump(), "DCDWCNS1RTKTEST00001", sample, start, now);
  REQUIRE(outcome.publication);
  CHECK(Json::parse(outcome.publication->payload)["error_code"] == "stale_lease");
}

TEST_CASE("pending expiry cancels receiver configuration before reporting its terminal state") {
  experiment::RtkBaseExperiment controller("cns_rpi");
  Json state{{"connected", true}, {"configuration", {{"state", "awaiting_ack"}}}};
  const auto sample = [&] { return state; };
  const auto start = [](auto, auto) -> std::expected<void, std::string> { return {}; };
  auto request = Request("rtk_base_survey_in");
  request["expires_at"] = "2026-11-06T00:00:00.200Z";
  const auto now = Wall::time_point{std::chrono::seconds{1793923200}};
  const auto accepted = controller.Handle(kTopic, request.dump(), "DCDWCNS1RTKTEST00001", sample, start, now);
  REQUIRE(accepted.publication);
  CHECK(Json::parse(accepted.publication->payload)["status"] == "accepted");
  const auto cancel = [&] { state["configuration"]["state"] = "failed"; };
  CHECK_FALSE(controller.Tick(sample, cancel, now + std::chrono::milliseconds{199}));
  CHECK(state["configuration"]["state"] == "awaiting_ack");
  const auto terminal = controller.Tick(sample, cancel, now + std::chrono::milliseconds{200});
  REQUIRE(terminal);
  const auto ack = Json::parse(terminal->payload);
  CHECK(ack["status"] == "failed");
  CHECK(ack["error_code"] == "configuration_timeout");
  CHECK(ack["fact"]["base"]["configuration"]["state"] == "failed");
  CHECK_FALSE(controller.Tick(sample, [] { FAIL("terminal actions cannot cancel again"); },
                              now + std::chrono::seconds{1}));
}
