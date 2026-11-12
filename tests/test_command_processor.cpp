#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "config_command/command_processor.hpp"

namespace {
nlohmann::json CurrentConfig(std::string applied_id = {}) {
  nlohmann::json ids = nlohmann::json::array();
  if (!applied_id.empty()) ids.push_back(applied_id);
  return {{"telemetry_publish",
           {{"snapshot", {{"enabled", true}, {"interval_ms", 1000}}},
            {"realtime", {{"enabled", true}, {"interval_ms", 100}}}}},
          {"runtime", {{"heartbeat_interval_ms", 1000},
                       {"applied_command_ids", ids}}},
          {"mqtt", {{"connection", {{"reconnect", {{"delay_s", 1},
                                                      {"delay_max_s", 30}}}}}}}};
}

std::string ValidPayload(std::string_view id) {
  return "{\"command_id\":\"" + std::string(id) +
         "\",\"parameters\":{\"heartbeat_interval_ms\":2000}}";
}
}  // namespace

TEST_CASE("成功持久化返回applied并要求退出") {
  bool persisted = false;
  auto result = config_command::ProcessConfigCommand(
      ValidPayload("cmd-new"), CurrentConfig(),
      [&](const nlohmann::json& candidate) {
        persisted = true;
        CHECK(candidate["runtime"]["heartbeat_interval_ms"] == 2000);
        return std::expected<void, config_command::CommandError>{};
      });
  CHECK(persisted);
  CHECK(result.ack == nlohmann::json{{"command_id", "cmd-new"},
                                     {"status", "applied"},
                                     {"restart_required", true}});
  CHECK(result.should_exit);
}

TEST_CASE("重复命令不写盘不退出") {
  bool persisted = false;
  auto result = config_command::ProcessConfigCommand(
      ValidPayload("cmd-old"), CurrentConfig("cmd-old"),
      [&](const nlohmann::json&) {
        persisted = true;
        return std::expected<void, config_command::CommandError>{};
      });
  CHECK_FALSE(persisted);
  CHECK(result.ack["command_id"] == "cmd-old");
  CHECK(result.ack["status"] == "already_applied");
  CHECK(result.ack["restart_required"] == false);
  CHECK_FALSE(result.should_exit);
}

TEST_CASE("解析和持久化失败均返回rejected并继续运行") {
  auto invalid = config_command::ProcessConfigCommand("{", CurrentConfig(),
      [](const nlohmann::json&) -> std::expected<void, config_command::CommandError> {
        FAIL("非法命令不应调用持久化");
        return {};
      });
  CHECK(invalid.ack["status"] == "rejected");
  CHECK(invalid.ack["error_code"] == "invalid_json");
  CHECK(invalid.ack["restart_required"] == false);
  CHECK_FALSE(invalid.should_exit);

  auto failed = config_command::ProcessConfigCommand(
      ValidPayload("cmd-fail"), CurrentConfig(),
      [](const nlohmann::json&) -> std::expected<void, config_command::CommandError> {
        return std::unexpected(config_command::CommandError{
            .code = "config_write_failed", .message = "磁盘写入失败"});
      });
  CHECK(failed.ack["command_id"] == "cmd-fail");
  CHECK(failed.ack["status"] == "rejected");
  CHECK(failed.ack["error_code"] == "config_write_failed");
  CHECK(failed.ack["message"] == "磁盘写入失败");
  CHECK_FALSE(failed.should_exit);
}

TEST_CASE("持久化状态不确定时返回rejected但要求重启核对磁盘") {
  auto result = config_command::ProcessConfigCommand(
      ValidPayload("cmd-uncertain"), CurrentConfig(),
      [](const nlohmann::json&) -> std::expected<void, config_command::CommandError> {
        return std::unexpected(config_command::CommandError{
            .code = "config_write_uncertain", .message = "配置提交状态不确定"});
      });
  CHECK(result.ack["status"] == "rejected");
  CHECK(result.ack["error_code"] == "config_write_uncertain");
  CHECK(result.ack["restart_required"] == true);
  CHECK(result.should_exit);
}

TEST_CASE("Topic周期补丁持久化后立即生效且不退出") {
  auto current = CurrentConfig();
  current["future_setting"] = {{"preserve", true}};
  nlohmann::json saved;
  const nlohmann::json intervals{
      {"telemetry/snapshot/v1", 2000}, {"telemetry/realtime/v1", 500},
      {"runtime/status/v1", 3000}, {"imu/sample/v1", 400}, {"gnss/raw/v1", 1000}};
  const auto payload = nlohmann::json{
      {"command_id", "rate-1"}, {"parameters", {{"topic_publish_intervals_ms", intervals}}}}.dump();
  const auto result = config_command::ProcessConfigCommand(
      payload, current, [&](const nlohmann::json& candidate) {
        saved = candidate;
        return std::expected<void, config_command::CommandError>{};
      });
  CHECK(result.ack["status"] == "applied");
  CHECK(result.ack["restart_required"] == false);
  CHECK(result.ack["topic_publish_intervals_ms"] == intervals);
  CHECK_FALSE(result.should_exit);
  REQUIRE_FALSE(saved.is_null());
  CHECK(saved["telemetry_publish"]["snapshot"]["interval_ms"] == 2000);
  CHECK(saved["telemetry_publish"]["realtime"]["interval_ms"] == 500);
  CHECK(saved["reporting"]["runtime_status_interval_ms"] == 3000);
  CHECK(saved["reporting"]["imu_sample_interval_ms"] == 400);
  CHECK(saved["reporting"]["gnss_batch_interval_ms"] == 1000);
  CHECK(saved["future_setting"] == current["future_setting"]);
  const auto duplicate = config_command::ProcessConfigCommand(
      payload, saved, [](const nlohmann::json&) {
        FAIL("重复调频命令不应写盘");
        return std::expected<void, config_command::CommandError>{};
      });
  CHECK(duplicate.ack["status"] == "already_applied");
  CHECK(duplicate.ack["topic_publish_intervals_ms"] == intervals);
  CHECK_FALSE(duplicate.should_exit);
}

TEST_CASE("调频识别配置中的真实Topic后缀并保留未修改周期") {
  auto current = CurrentConfig();
  current["mqtt"]["topics"] = {
      {"telemetry_snapshot", {{"suffix", "custom/snapshot"}}},
      {"telemetry_realtime", {{"suffix", "custom/realtime"}}}};
  nlohmann::json saved;
  const auto payload = nlohmann::json{{"command_id", "custom-rate"},
      {"parameters", {{"topic_publish_intervals_ms", {{"custom/realtime", 60000}}}}}}.dump();
  const auto result = config_command::ProcessConfigCommand(payload, current,
      [&](const nlohmann::json& candidate) {
        saved = candidate;
        return std::expected<void, config_command::CommandError>{};
      });
  CHECK(result.ack["status"] == "applied");
  CHECK(result.ack["topic_publish_intervals_ms"]["custom/realtime"] == 60000);
  CHECK(result.ack["topic_publish_intervals_ms"]["custom/snapshot"] == 1000);
  CHECK(result.ack["topic_publish_intervals_ms"]["imu/sample/v1"] == 0);
  saved["telemetry_publish"]["realtime"]["interval_ms"] = 500;
  const auto duplicate = config_command::ProcessConfigCommand(payload, saved,
      [](const nlohmann::json&) {
        FAIL("重投旧命令不应覆盖后来设置");
        return std::expected<void, config_command::CommandError>{};
      });
  CHECK(duplicate.ack["topic_publish_intervals_ms"]["custom/realtime"] == 500);
}

TEST_CASE("调频写失败和包含未知Topic的整条命令不产生部分应用") {
  const auto current = CurrentConfig();
  const auto payload = nlohmann::json{{"command_id", "rate-failed"},
      {"parameters", {{"topic_publish_intervals_ms", {{"imu/sample/v1", 1000}}}}}}.dump();
  const auto result = config_command::ProcessConfigCommand(payload, current,
      [](const nlohmann::json&) -> std::expected<void, config_command::CommandError> {
        return std::unexpected(config_command::CommandError{"config_write_failed", "write failed"});
      });
  CHECK(result.ack["status"] == "rejected");
  CHECK_FALSE(result.should_exit);
  CHECK_FALSE(current.contains("reporting"));
  auto invalid = nlohmann::json::parse(payload);
  invalid["parameters"]["topic_publish_intervals_ms"]["experiment/ack"] = 1000;
  CHECK(config_command::ProcessConfigCommand(invalid.dump(), current,
      [](const nlohmann::json&) {
        FAIL("非法Topic不得部分写盘");
        return std::expected<void, config_command::CommandError>{};
      }).ack["status"] == "rejected");
}

TEST_CASE("完整周期回执所需配置缺失时拒绝且不写盘不抛出") {
  for (const auto& id : {"new-rate", "duplicate-rate"}) {
    auto current = CurrentConfig("duplicate-rate");
    current["telemetry_publish"].erase("realtime");
    const auto payload = nlohmann::json{{"command_id", id},
        {"parameters", {{"topic_publish_intervals_ms", {{"gnss/raw/v1", 1000}}}}}}.dump();
    bool persisted = false;
    config_command::CommandProcessResult result;
    CHECK_NOTHROW(result = config_command::ProcessConfigCommand(payload, current,
        [&](const nlohmann::json&) {
          persisted = true;
          return std::expected<void, config_command::CommandError>{};
        }));
    CHECK_FALSE(persisted);
    REQUIRE(result.ack.is_object());
    CHECK(result.ack["status"] == "rejected");
    CHECK(result.ack["error_code"] == "config_read_failed");
    CHECK_FALSE(result.should_exit);
  }
}

TEST_CASE("运行和磁盘Topic后缀不一致时不能错写其他通道") {
  auto current = CurrentConfig();
  current["mqtt"]["topics"] = {
      {"telemetry_snapshot", {{"suffix", "disk/snapshot"}}},
      {"telemetry_realtime", {{"suffix", "telemetry/realtime/v1"}}}};
  const auto command = config_command::ParseConfigCommand(R"({"command_id":"topic-drift",
      "parameters":{"topic_publish_intervals_ms":{"running/snapshot":1000}}})",
      {"running/snapshot", "telemetry/realtime/v1"});
  REQUIRE(command);
  const auto result = config_command::ProcessConfigCommand(*command, current,
      [](const nlohmann::json&) {
        FAIL("磁盘配置尚未加载时不得保存调频补丁");
        return std::expected<void, config_command::CommandError>{};
      });
  CHECK(result.ack["status"] == "rejected");
  CHECK(result.ack["error_code"] == "config_read_failed");
  CHECK_FALSE(result.should_exit);
}
