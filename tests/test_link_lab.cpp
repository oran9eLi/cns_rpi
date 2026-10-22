#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "experiment/link_lab.hpp"

namespace {
using namespace std::chrono_literals;
constexpr auto kNow = std::chrono::sys_days{std::chrono::year{2026}/10/22} + 8h;
constexpr const char* kDevice = "DCDWCNS1S2MEAG1VTA0C";
constexpr const char* kSession = "c4191037-9e2a-480c-a995-2174e4672563";
constexpr const char* kAction = "f886f45b-01f8-4a54-b2b6-a65b07477aa1";

nlohmann::json Request(std::string operation) {
  return {{"schema_version", 1},
          {"command_id", "7d15e276-c7e1-4f52-aaee-7314a6417667"},
          {"request_id", kAction}, {"target", {{"device_id", kDevice}}},
          {"session_id", kSession}, {"action_id", kAction},
          {"lease_version", 3}, {"operation", operation},
          {"expires_at", "2026-10-22T08:00:30.000Z"}};
}

nlohmann::json Ack(const experiment::Outcome& result) {
  REQUIRE(result.publication.has_value());
  CHECK(result.publication->topic ==
        "cns_rpi/DCDWCNS1S2MEAG1VTA0C/experiment/ack");
  CHECK(result.publication->qos == 2);
  CHECK_FALSE(result.publication->retain);
  return nlohmann::json::parse(result.publication->payload);
}

experiment::LinkLabActions Actions(int& sends, int& changes) {
  return {.send_cellular = [&](bool) { ++sends; return true; },
          .apply_baud = [&](int baud) -> std::optional<int> {
            ++changes;
            return baud;
          }};
}
}  // namespace

TEST_CASE("首个教学断开必须真实发送RPICELL且不改变持久波特率") {
  experiment::LinkLabController lab("cns_rpi", kDevice, 115200);
  int sends = 0, changes = 0;
  auto request = Request("link_lab_set_cellular");
  request["state"] = "disconnected";
  const auto ack = Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow));
  CHECK(ack.at("status") == "completed");
  CHECK(ack.at("fact") == nlohmann::json{{"state", "disconnected"},
                                         {"report_sent", true}});
  CHECK(sends == 1);
  CHECK(changes == 0);
  CHECK(lab.CellularOffline());
  CHECK(lab.CurrentBaud() == 115200);
}

TEST_CASE("断开状态写帧失败必须拒绝且不改变覆盖") {
  experiment::LinkLabController lab("cns_rpi", kDevice, 115200);
  auto request = Request("link_lab_set_cellular");
  request["state"] = "disconnected";
  const auto ack = Ack(lab.Handle(request.dump(),
      {.send_cellular = [](bool) { return false; }, .apply_baud = {}}, kNow));
  CHECK(ack.at("status") == "rejected");
  CHECK(ack.at("error_code") == "serial_unavailable");
  CHECK_FALSE(ack.contains("fact"));
  CHECK_FALSE(lab.CellularOffline());
}

TEST_CASE("临时换速只改变进程目标且重投不重复执行") {
  experiment::LinkLabController lab("cns_rpi", kDevice, 115200);
  int sends = 0, changes = 0;
  auto request = Request("link_lab_set_baud");
  request["baud_rate"] = 57600;
  const auto actions = Actions(sends, changes);
  const auto first = Ack(lab.Handle(request.dump(), actions, kNow));
  CHECK(first.at("fact") == nlohmann::json{{"baud_rate", 57600}});
  CHECK(lab.CurrentBaud() == 57600);
  request["command_id"] = "58ad6990-6d65-4d9a-9f53-82d611d0e3f4";
  const auto replay = Ack(lab.Handle(request.dump(), actions, kNow + 1s));
  CHECK(replay.at("fact") == first.at("fact"));
  CHECK(replay.at("command_id") == request.at("command_id"));
  CHECK(changes == 1);
  request["baud_rate"] = 115200;
  const auto conflict = Ack(lab.Handle(request.dump(), actions, kNow + 2s));
  CHECK(conflict.at("error_code") == "duplicate_conflict");
  CHECK(changes == 1);
}

TEST_CASE("去重指纹兼容缺省可选字段且不混淆不同动作") {
  experiment::LinkLabController lab("cns_rpi", kDevice, 115200);
  int sends = 0, changes = 0;
  auto request = Request("link_lab_restore");
  const auto actions = Actions(sends, changes);
  const auto first = Ack(lab.Handle(request.dump(), actions, kNow));
  CHECK(first.at("status") == "completed");
  request["command_id"] = "58ad6990-6d65-4d9a-9f53-82d611d0e3f4";
  const auto replay = Ack(lab.Handle(request.dump(), actions, kNow + 1s));
  CHECK(replay.at("status") == "completed");
  CHECK(replay.at("fact") == first.at("fact"));
  request["operation"] = "link_lab_set_status_reporting";
  request["enabled"] = false;
  const auto conflict = Ack(lab.Handle(request.dump(), actions, kNow + 2s));
  CHECK(conflict.at("error_code") == "duplicate_conflict");
  CHECK(lab.ReportingEnabled());
}

TEST_CASE("状态报送开关和恢复不依赖当前F407身份") {
  experiment::LinkLabController lab("cns_rpi", kDevice, 115200);
  int sends = 0, changes = 0;
  auto request = Request("link_lab_set_status_reporting");
  request["enabled"] = false;
  CHECK(Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow))
            .at("fact").at("enabled") == false);
  CHECK_FALSE(lab.ReportingEnabled());
  request["action_id"] = request["request_id"] =
      "58ad6990-6d65-4d9a-9f53-82d611d0e3f4";
  request["operation"] = "link_lab_restore";
  request.erase("enabled");
  const auto ack = Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow));
  CHECK(ack.at("fact") == nlohmann::json{{"restored_baud_rate", 115200},
                                         {"cellular_override_cleared", true},
                                         {"status_reporting_enabled", true}});
  CHECK(lab.ReportingEnabled());
}

TEST_CASE("过期动作与旧租约不能覆盖新会话") {
  experiment::LinkLabController lab("cns_rpi", kDevice, 115200);
  int sends = 0, changes = 0;
  auto request = Request("link_lab_set_status_reporting");
  request["enabled"] = false;
  CHECK(Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow + 31s))
            .at("error_code") == "action_expired");
  CHECK(lab.ReportingEnabled());
  request["lease_version"] = 4;
  request["session_id"] = "58ad6990-6d65-4d9a-9f53-82d611d0e3f4";
  request["action_id"] = request["request_id"] =
      "a514719a-35e0-4c4b-b0a3-51fd972393ed";
  CHECK(Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow))
            .at("status") == "completed");
  request["lease_version"] = 3;
  request["session_id"] = kSession;
  request["action_id"] = request["request_id"] =
      "4691173f-aa4a-4ad2-9a75-c72921d706f3";
  CHECK(Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow))
            .at("error_code") == "action_expired");
}

TEST_CASE("重启清除临时覆盖并拒绝上次进程的旧会话") {
  const auto path = std::filesystem::temp_directory_path() /
                    "cns-link-lab-test-fence.json";
  std::filesystem::remove(path);
  int sends = 0, changes = 0;
  auto request = Request("link_lab_set_baud");
  request["baud_rate"] = 57600;
  {
    experiment::LinkLabController lab("cns_rpi", kDevice, 115200, path);
    CHECK(lab.Ready());
    CHECK(Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow))
              .at("status") == "completed");
  }
  {
    experiment::LinkLabController lab("cns_rpi", kDevice, 115200, path);
    CHECK(lab.Ready());
    CHECK(lab.CurrentBaud() == 115200);
    CHECK_FALSE(lab.CellularOffline());
    CHECK(lab.ReportingEnabled());
    request["action_id"] = request["request_id"] =
        "a514719a-35e0-4c4b-b0a3-51fd972393ed";
    CHECK(Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow))
              .at("error_code") == "action_expired");
  }
  std::filesystem::remove(path);
}

TEST_CASE("控制链中断或租约超时清除覆盖并封闭旧会话") {
  experiment::LinkLabController lab("cns_rpi", kDevice, 115200);
  int sends = 0, changes = 0;
  auto request = Request("link_lab_set_baud");
  request["baud_rate"] = 57600;
  const auto start = std::chrono::steady_clock::time_point{10s};
  CHECK(Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow, start))
            .at("status") == "completed");
  CHECK_FALSE(lab.SessionExpired(start + 299s));
  CHECK(lab.SessionExpired(start + 300s));
  lab.Abort();
  CHECK(lab.CurrentBaud() == 115200);
  CHECK_FALSE(lab.CellularOffline());
  CHECK(lab.ReportingEnabled());
  CHECK(Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow, start + 1s))
            .at("error_code") == "action_expired");
  request["action_id"] = request["request_id"] =
      "4691173f-aa4a-4ad2-9a75-c72921d706f3";
  CHECK(Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow, start + 1s))
            .at("error_code") == "action_expired");
}

TEST_CASE("教学断开只清除RPICELL在线位而保留真实链路诊断位") {
  experiment::LinkLabController lab("cns_rpi", kDevice, 115200);
  int sends = 0, changes = 0;
  auto request = Request("link_lab_set_cellular");
  request["state"] = "disconnected";
  CHECK(Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow))
            .at("status") == "completed");
  cellular::LinkStatus actual{.interface_present = true, .carrier_up = true,
                              .has_ip_address = true, .has_default_route = true,
                              .online = true};
  const auto teaching = lab.TeachingCellularStatus(actual);
  CHECK_FALSE(teaching.online);
  CHECK(teaching.interface_present);
  CHECK(teaching.carrier_up);
  CHECK(teaching.has_ip_address);
  CHECK(teaching.has_default_route);
  CHECK(actual.online);
  CHECK(cellular::PackRpiCellularValue(teaching) == 30);
  CHECK(cellular::PackRpiCellularValue(actual) == 31);
}

TEST_CASE("严格校验不支持的波特率、额外字段和目标身份") {
  experiment::LinkLabController lab("cns_rpi", kDevice, 115200);
  int sends = 0, changes = 0;
  auto request = Request("link_lab_set_baud");
  request["baud_rate"] = 9600;
  CHECK_FALSE(lab.Handle(request.dump(), Actions(sends, changes), kNow)
                  .publication.has_value());
  request["baud_rate"] = 57600;
  request["unexpected"] = 1;
  CHECK_FALSE(lab.Handle(request.dump(), Actions(sends, changes), kNow)
                  .publication.has_value());
  request.erase("unexpected");
  request["target"]["device_id"] = "DCDWCNS1GHC0G6LF8MY6";
  CHECK_FALSE(lab.Handle(request.dump(), Actions(sends, changes), kNow)
                  .publication.has_value());
  CHECK(changes == 0);
}

TEST_CASE("5G恢复在串口失配时仍可完成且如实报告未发送") {
  experiment::LinkLabController lab("cns_rpi", kDevice, 115200);
  int sends = 0, changes = 0;
  auto request = Request("link_lab_set_cellular");
  request["state"] = "disconnected";
  CHECK(Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow))
            .at("status") == "completed");
  request["action_id"] = request["request_id"] =
      "4691173f-aa4a-4ad2-9a75-c72921d706f3";
  request["state"] = "normal";
  const auto ack = Ack(lab.Handle(request.dump(),
      {.send_cellular = [](bool) { return false; }, .apply_baud = {}}, kNow));
  CHECK(ack.at("status") == "completed");
  CHECK(ack.at("fact") == nlohmann::json{{"state", "normal"},
                                         {"report_sent", false}});
  CHECK_FALSE(lab.CellularOffline());
}

TEST_CASE("临时换速读回失败不宣称完成，恢复命令清除全部覆盖") {
  experiment::LinkLabController lab("cns_rpi", kDevice, 115200);
  int sends = 0, changes = 0;
  auto request = Request("link_lab_set_baud");
  request["baud_rate"] = 57600;
  CHECK(Ack(lab.Handle(request.dump(),
      {.send_cellular = {}, .apply_baud = [](int) -> std::optional<int> {
         return std::nullopt;
       }}, kNow)).at("error_code") == "serial_unavailable");
  CHECK(lab.CurrentBaud() == 115200);
  request["action_id"] = request["request_id"] =
      "4691173f-aa4a-4ad2-9a75-c72921d706f3";
  CHECK(Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow))
            .at("status") == "completed");
  request["action_id"] = request["request_id"] =
      "a514719a-35e0-4c4b-b0a3-51fd972393ed";
  request["operation"] = "link_lab_restore";
  request.erase("baud_rate");
  CHECK(Ack(lab.Handle(request.dump(), Actions(sends, changes), kNow))
            .at("status") == "completed");
  CHECK(lab.CurrentBaud() == 115200);
  CHECK(changes == 2);
}
