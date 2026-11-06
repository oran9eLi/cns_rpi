#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "experiment/video_request.hpp"

namespace {
using Json = nlohmann::json;

Json Request(std::string operation = "video_start") {
  Json result = {{"schema_version", 1},
      {"command_id", "7d15e276-c7e1-4f52-aaee-7314a6417667"},
      {"request_id", "f886f45b-01f8-4a54-b2b6-a65b07477aa1"},
      {"action_id", "f886f45b-01f8-4a54-b2b6-a65b07477aa1"},
      {"session_id", "c4191037-9e2a-480c-a995-2174e4672563"},
      {"target", {{"device_id", "DCDWCNS1S2MEAG1VTA0C"}}},
      {"lease_version", 3}, {"operation", operation},
      {"expires_at", "2026-11-05T12:00:30.000Z"}};
  if (operation == "video_start" || operation == "video_test") {
    result["parameters"] = {{"camera", 0}, {"width", 1280}, {"height", 720},
        {"framerate", 25}, {"bitrate_bps", 2000000},
        {"duration_seconds", operation == "video_test" ? 3 : 900}};
  }
  if (operation == "video_start") {
    result["destination"] = {{"host", "media.example.edu"}, {"port", 8890},
        {"stream_path", "g2/session-03/g2-03"}};
  }
  return result;
}
}  // namespace

TEST_CASE("识别五个视频操作而不截获旧实验") {
  for (const auto* operation : {"video_probe", "video_test", "video_start", "video_stop", "video_status"}) {
    CHECK(experiment::IsVideoOperation(Request(operation).dump()));
  }
  CHECK_FALSE(experiment::IsVideoOperation(Request("inspect_pi_link").dump()));
  CHECK_FALSE(experiment::IsVideoOperation("not-json"));
}

TEST_CASE("视频开始请求保留类型化参数和设备会话关联") {
  const auto parsed = experiment::ParseVideoRequest(Request().dump());
  REQUIRE(parsed);
  CHECK(parsed->video.kind == video::Kind::kPublish);
  CHECK(parsed->video.parameters.width == 1280);
  CHECK(parsed->video.parameters.framerate == 25);
  CHECK(parsed->video.parameters.bitrate_bps == 2000000);
  CHECK(parsed->video.duration == std::chrono::seconds(900));
  CHECK(parsed->video.destination.host == "media.example.edu");
  CHECK(parsed->video.destination.stream_path == "g2/session-03/g2-03");
  CHECK(parsed->lease_version == 3);
}

TEST_CASE("视频请求拒绝越界非整型与未知字段") {
  for (const auto& [field, value] : std::vector<std::pair<std::string, Json>>{
           {"width", 1279}, {"height", 0}, {"framerate", 31},
           {"bitrate_bps", 1}, {"camera", -1}, {"duration_seconds", 1801},
           {"width", "1280"}, {"width", 1280.5}}) {
    auto request = Request();
    request["parameters"][field] = value;
    CHECK_FALSE(experiment::ParseVideoRequest(request.dump()));
  }
  auto request = Request();
  request["command"] = "anything";
  CHECK_FALSE(experiment::ParseVideoRequest(request.dump()));
  request = Request("video_test");
  request["parameters"]["duration_seconds"] = 11;
  CHECK_FALSE(experiment::ParseVideoRequest(request.dump()));
}

TEST_CASE("视频目的地是结构化字段且不能夹带URL参数或路径穿越") {
  for (const auto* host : {"srt://host", "host?streamid=x", "host name", "-bad", "bad..host"}) {
    auto request = Request();
    request["destination"]["host"] = host;
    CHECK_FALSE(experiment::ParseVideoRequest(request.dump()));
  }
  for (const auto* path : {"/g2", "../g2", "g2/../other", "g2?x", "g2//other", "g2/"}) {
    auto request = Request();
    request["destination"]["stream_path"] = path;
    CHECK_FALSE(experiment::ParseVideoRequest(request.dump()));
  }
}

TEST_CASE("视频沿用动作UUID目标租约和严格UTC时间约束") {
  for (const auto& [field, value] : std::vector<std::pair<std::string, Json>>{
           {"schema_version", 2}, {"lease_version", -1}, {"lease_version", 0},
           {"command_id", "not-uuid"}, {"request_id", "7d15e276-c7e1-4f52-aaee-7314a6417667"},
           {"expires_at", "2026-02-30T12:00:30.000Z"}, {"expires_at", "2026-11-05T12:00:30"},
           {"expires_at", "2026-11-05T-0:00:30.000Z"}}) {
    auto request = Request();
    request[field] = value;
    CHECK_FALSE(experiment::ParseVideoRequest(request.dump()));
  }
}

TEST_CASE("仅command_id变化可复用同动作结果") {
  auto first = Request();
  auto retry = first;
  retry["command_id"] = "f347f0c2-1f3e-4fb2-bc0a-00dff10b0001";
  const auto a = experiment::ParseVideoRequest(first.dump());
  const auto b = experiment::ParseVideoRequest(retry.dump());
  REQUIRE(a);
  REQUIRE(b);
  CHECK(a->comparison == b->comparison);
  retry["parameters"]["bitrate_bps"] = 4000000;
  const auto changed = experiment::ParseVideoRequest(retry.dump());
  REQUIRE(changed);
  CHECK(a->comparison != changed->comparison);
}
