#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "config_command/config_updater.hpp"

namespace {
nlohmann::json CurrentConfig() {
  return {
      {"serial", {{"device", "/dev/ttyUSB0"}, {"baud", 115200}}},
      {"runtime", {{"telemetry_publish_interval_ms", 1000},
                   {"heartbeat_interval_ms", 1000},
                   {"applied_command_ids", nlohmann::json::array()}}},
      {"mqtt", {{"connection", {{"reconnect", {{"delay_s", 1},
                                                  {"delay_max_s", 30}}}}}}},
      {"cellular", {{"apn", "cmnet"}}},
      {"custom_future_field", {{"enabled", true}}},
  };
}
}  // namespace

TEST_CASE("部分更新保留全部非白名单字段并追加命令号") {
  auto current = CurrentConfig();
  config_command::ConfigParameterPatch patch;
  patch.telemetry_publish_interval_ms = 2000;
  config_command::ConfigCommand command{
      .command_id = "cmd-002",
      .parameters = patch};
  auto result = config_command::BuildUpdatedConfig(current, command);
  REQUIRE(result.has_value());
  CHECK((*result)["runtime"]["telemetry_publish_interval_ms"] == 2000);
  CHECK((*result)["runtime"]["heartbeat_interval_ms"] == 1000);
  CHECK((*result)["serial"] == current["serial"]);
  CHECK((*result)["cellular"] == current["cellular"]);
  CHECK((*result)["custom_future_field"] == current["custom_future_field"]);
  CHECK((*result)["runtime"]["applied_command_ids"].back() == "cmd-002");
}

TEST_CASE("幂等窗口只保留最近32条") {
  auto current = CurrentConfig();
  for (int i = 0; i < 32; ++i) {
    current["runtime"]["applied_command_ids"].push_back("cmd-" + std::to_string(i));
  }
  config_command::ConfigParameterPatch patch;
  patch.heartbeat_interval_ms = 2000;
  config_command::ConfigCommand command{.command_id = "cmd-32", .parameters = patch};
  auto result = config_command::BuildUpdatedConfig(current, command);
  REQUIRE(result.has_value());
  const auto& ids = (*result)["runtime"]["applied_command_ids"];
  CHECK(ids.size() == 32);
  CHECK(ids.front() == "cmd-1");
  CHECK(ids.back() == "cmd-32");
  CHECK(config_command::IsCommandApplied(*result, "cmd-32"));
  CHECK_FALSE(config_command::IsCommandApplied(*result, "cmd-0"));
}

TEST_CASE("单独修改重连初始等待时检查最终组合") {
  auto current = CurrentConfig();
  config_command::ConfigParameterPatch patch;
  patch.mqtt_reconnect_delay_s = 31;
  config_command::ConfigCommand command{.command_id = "cmd-bad", .parameters = patch};
  auto result = config_command::BuildUpdatedConfig(current, command);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == "invalid_parameter");
  CHECK(current["mqtt"]["connection"]["reconnect"]["delay_s"] == 1);
}

TEST_CASE("从文件读取完整JSON时区分打不开与格式损坏") {
  CHECK(config_command::LoadConfigJson("/nonexistent/config.json").error().code ==
        "config_read_failed");
  auto path = std::filesystem::temp_directory_path() / "cns_rpi_invalid_update.json";
  std::ofstream(path) << "{";
  CHECK(config_command::LoadConfigJson(path).error().code == "config_read_failed");
  std::filesystem::remove(path);
}
