#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fstream>
#include <thread>
#include <vector>
#include <unistd.h>
#include <nlohmann/json.hpp>

#include "command_feedback/result_feedback.hpp"
#include "experiment/video_experiment.hpp"

namespace {
using namespace std::chrono_literals;
using Json = nlohmann::json;
constexpr auto kWall = std::chrono::sys_days{std::chrono::year{2026}/11/5} + 12h;
constexpr const char* kDevice = "DCDWCNS1S2MEAG1VTA0C";
constexpr const char* kTopic = "cns_rpi/DCDWCNS1S2MEAG1VTA0C/experiment/set";
Json Request(std::string operation, int action = 1) {
  auto id = std::string("f886f45b-01f8-4a54-b2b6-a65b07477aa") + std::to_string(action);
  Json request{{"schema_version", 1}, {"command_id", "7d15e276-c7e1-4f52-aaee-7314a6417667"},
      {"request_id", id}, {"action_id", id}, {"session_id", "c4191037-9e2a-480c-a995-2174e4672563"},
      {"target", {{"device_id", kDevice}}}, {"lease_version", 3}, {"operation", operation},
      {"expires_at", "2026-11-05T12:00:30.000Z"}};
  if (operation == "video_test" || operation == "video_start") {
    request["parameters"] = {{"camera", 0}, {"width", 1280}, {"height", 720},
        {"framerate", 25}, {"bitrate_bps", 2000000}, {"duration_seconds", 3}};
  }
  if (operation == "video_start") request["destination"] = {
      {"host", "media.example.edu"}, {"port", 8890}, {"stream_path", "g2/group-03"}};
  return request;
}
experiment::Observation Online() {
  return {.runtime = {.device_id = kDevice,
                      .identity_status = runtime_status::IdentityStatus::kCached},
          .serial_port_open = std::nullopt, .mqtt_connected = true};
}
struct Fixture {
  std::filesystem::path directory = std::filesystem::temp_directory_path() /
      ("cns-video-controller-" + std::to_string(getpid()));
  Fixture() {
    std::filesystem::create_directories(directory);
    std::ofstream script(directory / "camera");
    script << "#!/bin/sh\ncase \"$*\" in *--list-cameras*) echo '0 : imx708_wide [4608x2592]'; exit 0;; esac\n"
              "echo 'Viewfinder frame 0' >&2\nexec sleep 60\n";
    script.close();
    std::filesystem::permissions(directory / "camera", std::filesystem::perms::owner_all);
  }
  ~Fixture() { std::filesystem::remove_all(directory); }
  video::Settings Settings() const { return {(directory / "camera").string(), "/usr/bin/ffprobe", directory}; }
};
experiment::Outcome Handle(experiment::VideoExperiment& controller, const Json& request,
    const experiment::PiLinkInspector::Sampler& sample = [] { return Online(); }) {
  return controller.Handle(kTopic, request.dump(),
      device::Binding{.device_id = kDevice, .device_type = device::Type::kCnsBox},
      sample, kWall, video::Clock::now());
}
Json Ack(const experiment::Outcome& result) {
  REQUIRE(result.publication);
  CHECK(result.publication->qos == 2);
  CHECK_FALSE(result.publication->retain);
  CHECK(result.publication->topic == "cns_rpi/DCDWCNS1S2MEAG1VTA0C/experiment/ack");
  return Json::parse(result.publication->payload);
}
std::optional<experiment::Publication> Poll(experiment::VideoExperiment& controller, bool can_publish = true) {
  std::optional<experiment::Publication> result;
  for (int i = 0; i < 100 && !result; ++i) {
    result = controller.Tick(kWall, video::Clock::now(), can_publish);
    std::this_thread::sleep_for(2ms);
  }
  return result;
}
}

TEST_CASE("视频默认关闭且过期和身份错误不能启动摄像头") {
  video::Manager manager({"/missing-camera", "/usr/bin/ffprobe", "/tmp/cns-video-disabled"});
  experiment::VideoExperiment disabled("cns_rpi", false, manager);
  CHECK(Ack(Handle(disabled, Request("video_start")))["error_code"] == "video_disabled");
  experiment::VideoExperiment enabled("cns_rpi", true, manager);
  auto expired = Request("video_start"); expired["expires_at"] = "2026-11-05T11:59:59.000Z";
  CHECK(Ack(Handle(enabled, expired))["error_code"] == "action_expired");
  CHECK_FALSE(manager.Busy());
  auto conflict = [] { auto observation = Online();
    observation.runtime.identity_status = runtime_status::IdentityStatus::kConflict; return observation; };
  CHECK(Ack(Handle(enabled, Request("video_start", 2), conflict))["error_code"] == "identity_unavailable");
  CHECK_FALSE(manager.Busy());
}

TEST_CASE("业务离线可查询视频且终态重投复用观测结果") {
  video::Manager manager({"/missing-camera", "/usr/bin/ffprobe", "/tmp/cns-video-status"});
  experiment::VideoExperiment controller("cns_rpi", true, manager);
  auto request = Request("video_status");
  const auto first = Ack(Handle(controller, request));
  CHECK(first["status"] == "completed");
  CHECK(first["fact"]["state"] == "idle");
  request["command_id"] = "7d15e276-c7e1-4f52-aaee-7314a6417668";
  const auto replay = Ack(Handle(controller, request, [] { return std::nullopt; }));
  CHECK(replay["fact"] == first["fact"]);
  CHECK(replay["observed_at"] == first["observed_at"]);
  CHECK(replay["command_id"] == request["command_id"]);
  request["lease_version"] = 4;
  CHECK(Ack(Handle(controller, request))["error_code"] == "duplicate_conflict");
}

TEST_CASE("启动只报告进程事实且重投不启动第二个进程") {
  Fixture fixture; video::Manager manager(fixture.Settings());
  experiment::VideoExperiment controller("cns_rpi", true, manager);
  auto request = Request("video_start");
  const auto first = Ack(Handle(controller, request));
  CHECK(first["status"] == "completed");
  CHECK(first["fact"]["encoded_frames"].is_null());
  CHECK(first["fact"]["receiver_observed"].is_null());
  const auto pid = manager.Current().pid;
  REQUIRE(pid > 0);
  request["command_id"] = "7d15e276-c7e1-4f52-aaee-7314a6417668";
  CHECK(Ack(Handle(controller, request))["fact"] == first["fact"]);
  CHECK(manager.Current().pid == pid);
  CHECK(Ack(Handle(controller, Request("video_test", 2)))["error_code"] == "camera_busy");
  auto foreign = Request("video_stop", 3);
  foreign["session_id"] = "c4191037-9e2a-480c-a995-2174e4672564";
  CHECK(Ack(Handle(controller, foreign))["error_code"] == "session_conflict");
  auto stale = Request("video_stop", 4); stale["lease_version"] = 2;
  CHECK(Ack(Handle(controller, stale))["error_code"] == "stale_lease");
  CHECK_FALSE(Handle(controller, Request("video_stop", 5)).publication);
  const auto stopped = Poll(controller);
  REQUIRE(stopped);
  CHECK(Json::parse(stopped->payload)["status"] == "completed");
  CHECK_FALSE(manager.Busy());
}

TEST_CASE("探测等待实际退出并在ACK背压下保留终态") {
  Fixture fixture; video::Manager manager(fixture.Settings());
  experiment::VideoExperiment controller("cns_rpi", true, manager);
  auto request = Request("video_probe");
  CHECK_FALSE(controller.Busy());
  CHECK_FALSE(Handle(controller, request).publication);
  CHECK(controller.Busy());
  request["command_id"] = "7d15e276-c7e1-4f52-aaee-7314a6417668";
  CHECK_FALSE(Handle(controller, request).publication);
  CHECK_FALSE(Poll(controller, false));
  CHECK_FALSE(manager.Busy());
  CHECK(controller.Busy());
  const auto result = controller.Tick(kWall, video::Clock::now(), true);
  REQUIRE(result);
  const auto ack = Json::parse(result->payload);
  CHECK(ack["command_id"] == request["command_id"]);
  CHECK(ack["fact"]["cameras"][0]["model"] == "imx708_wide");
  CHECK(ack["status"] == "completed");
  CHECK_FALSE(controller.Tick(kWall, video::Clock::now(), true));
  CHECK_FALSE(controller.Busy());
}

TEST_CASE("视频终态在ACK队列满时立即通知且之后发布不重放提示") {
  Fixture fixture; video::Manager manager(fixture.Settings());
  command_feedback::ResultFeedback feedback;
  std::vector<Json> observed;
  experiment::VideoExperiment controller("cns_rpi", true, manager, [&](const Json& ack) {
    observed.push_back(ack);
    CHECK(feedback.Observe(kDevice, command_feedback::Channel::kExperiment, ack));
  });
  CHECK_FALSE(Handle(controller, Request("video_probe")).publication);
  CHECK_FALSE(Poll(controller, false));
  REQUIRE(observed.size() == 1);
  CHECK(observed.front()["status"] == "completed");
  const auto publication = controller.Tick(kWall, video::Clock::now(), true);
  REQUIRE(publication);
  CHECK(Json::parse(publication->payload) == observed.front());
  CHECK_FALSE(feedback.Observe(kDevice, command_feedback::Channel::kExperiment,
                              Json::parse(publication->payload)));
  CHECK_FALSE(controller.Tick(kWall, video::Clock::now(), true));
  CHECK(observed.size() == 1);
}

TEST_CASE("停止试采集保留原动作取消及停止动作的各自终态") {
  Fixture fixture; video::Manager manager(fixture.Settings());
  experiment::VideoExperiment controller("cns_rpi", true, manager);
  CHECK_FALSE(Handle(controller, Request("video_test")).publication);
  CHECK_FALSE(Handle(controller, Request("video_stop", 2)).publication);
  const auto cancelled = Poll(controller);
  REQUIRE(cancelled);
  CHECK(Json::parse(cancelled->payload)["error_code"] == "cancelled");
  const auto stopped = Poll(controller);
  REQUIRE(stopped);
  CHECK(Json::parse(stopped->payload)["operation"] == "video_stop");
  CHECK(Json::parse(stopped->payload)["status"] == "completed");
}

TEST_CASE("无绑定错误Topic与错误目标均无摄像头副作用") {
  Fixture fixture; video::Manager manager(fixture.Settings());
  experiment::VideoExperiment controller("cns_rpi", true, manager);
  const auto request = Request("video_start");
  CHECK_FALSE(controller.Handle(kTopic, request.dump(), std::nullopt, [] { return Online(); },
                                kWall, video::Clock::now()).publication);
  CHECK_FALSE(controller.Handle("wrong/topic", request.dump(),
      device::Binding{.device_id = kDevice, .device_type = device::Type::kCnsBox},
      [] { return Online(); }, kWall, video::Clock::now()).publication);
  auto wrong = request;
  wrong["target"]["device_id"] = "DCDWCNS1GHC0G6LF8MY6";
  CHECK(Ack(Handle(controller, wrong))["error_code"] == "identity_unavailable");
  CHECK_FALSE(manager.Busy());
}

TEST_CASE("启动工具缺失返回真实失败且新动作可以重试") {
  video::Manager manager({"/missing-camera", "/usr/bin/ffprobe", "/tmp/cns-video-missing"});
  experiment::VideoExperiment controller("cns_rpi", true, manager);
  const auto ack = Ack(Handle(controller, Request("video_start")));
  CHECK(ack["status"] == "rejected");
  CHECK(ack["error_code"] == "start_failed");
  CHECK(ack["fact"]["state"] == "failed");
  CHECK_FALSE(manager.Busy());
  CHECK(Ack(Handle(controller, Request("video_start", 2)))["error_code"] == "start_failed");
}

TEST_CASE("设备租约跨Session单调且相同版本不能换Session") {
  Fixture fixture; video::Manager manager(fixture.Settings());
  experiment::VideoExperiment controller("cns_rpi", true, manager);
  auto current = Request("video_probe");
  current["lease_version"] = 11;
  CHECK_FALSE(Handle(controller, current).publication);
  REQUIRE(Poll(controller));
  auto old = Request("video_start", 2);
  old["session_id"] = "c4191037-9e2a-480c-a995-2174e4672564";
  old["lease_version"] = 10;
  CHECK(Ack(Handle(controller, old))["error_code"] == "stale_lease");
  old["action_id"] = "f886f45b-01f8-4a54-b2b6-a65b07477aa3";
  old["request_id"] = old["action_id"];
  old["lease_version"] = 11;
  CHECK(Ack(Handle(controller, old))["error_code"] == "stale_lease");
  CHECK_FALSE(manager.Busy());
}

TEST_CASE("不同Session的历史查询和新启动失败具有明确归属") {
  Fixture fixture; video::Manager manager(fixture.Settings());
  experiment::VideoExperiment controller("cns_rpi", true, manager);
  CHECK_FALSE(Handle(controller, Request("video_probe")).publication);
  REQUIRE(Poll(controller));
  auto next = Request("video_status", 2);
  next["session_id"] = "c4191037-9e2a-480c-a995-2174e4672564";
  next["lease_version"] = 4;
  const auto previous = Ack(Handle(controller, next));
  CHECK(previous["fact"].value("belongs_to_session", true) == false);
  CHECK(previous["fact"]["run_id"] == Request("video_probe")["action_id"]);
  next = Request("video_start", 3);
  next["session_id"] = "c4191037-9e2a-480c-a995-2174e4672564";
  next["lease_version"] = 4;
  std::filesystem::remove(fixture.directory / "camera");
  const auto failure = Ack(Handle(controller, next));
  CHECK(failure["error_code"] == "start_failed");
  CHECK(failure["fact"]["run_id"] == next["action_id"]);
  CHECK(failure["fact"]["owner_session_id"] == next["session_id"]);
  CHECK(failure["fact"]["belongs_to_session"] == true);
}
