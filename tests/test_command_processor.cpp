#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "config_command/command_processor.hpp"

namespace {
nlohmann::json CurrentConfig(std::string applied_id = {}) {
  nlohmann::json ids = nlohmann::json::array();
  if (!applied_id.empty()) ids.push_back(applied_id);
  return {{"runtime", {{"telemetry_publish_interval_ms", 1000},
                       {"heartbeat_interval_ms", 1000},
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
