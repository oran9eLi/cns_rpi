#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "experiment/pi_baud_change.hpp"

namespace {
struct Fixture {
  std::filesystem::path path;
  Fixture() {
    static int serial = 0;
    path = std::filesystem::temp_directory_path() /
           ("cns-m1-pi-baud-" + std::to_string(++serial) + ".json");
    std::ofstream(path) << R"({"serial":{"device":"auto","baud":115200},"mqtt":{"keep":1}})";
  }
  ~Fixture() { std::filesystem::remove(path); }
};
}  // namespace

TEST_CASE("Pi在线换速只改串口速率并在持久化后独占重开") {
  Fixture f;
  bool persisted = false;
  bool closed = false;
  const auto result = experiment::ApplyPiBaud(
      f.path, 57600,
      [&](const nlohmann::json& candidate) ->
          std::expected<void, config_command::CommandError> {
        CHECK(candidate.at("mqtt").at("keep") == 1);
        CHECK(candidate.at("serial").at("device") == "auto");
        CHECK(candidate.at("serial").at("baud") == 57600);
        persisted = true;
        return {};
      },
      [&] { CHECK(persisted); closed = true; },
      [&](int baud) -> std::expected<int, std::string> {
        CHECK(closed);
        CHECK(baud == 57600);
        return 57600;
      });
  REQUIRE(result.has_value());
  CHECK(result->applied_baud == 57600);
}

TEST_CASE("配置写失败不能关闭原串口或声称改参成功") {
  Fixture f;
  bool closed = false;
  const auto result = experiment::ApplyPiBaud(
      f.path, 57600,
      [](const nlohmann::json&) ->
          std::expected<void, config_command::CommandError> {
        return std::unexpected(config_command::CommandError{
            .code = "config_write_failed", .message = "模拟写失败"});
      },
      [&] { closed = true; },
      [](int) -> std::expected<int, std::string> { return 57600; });
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == "config_write_failed");
  CHECK_FALSE(closed);
}

TEST_CASE("配置已保存但重开或读回失败不能发布成功事实") {
  Fixture f;
  const auto persist = [](const nlohmann::json&) ->
      std::expected<void, config_command::CommandError> { return {}; };
  const auto open_failed = experiment::ApplyPiBaud(
      f.path, 57600, persist, [] {},
      [](int) -> std::expected<int, std::string> {
        return std::unexpected("串口重新打开失败");
      });
  REQUIRE_FALSE(open_failed.has_value());
  CHECK(open_failed.error().code == "serial_reconfigure_failed");
  CHECK(open_failed.error().configuration_persisted);
  const auto wrong_rate = experiment::ApplyPiBaud(
      f.path, 57600, persist, [] {},
      [](int) -> std::expected<int, std::string> { return 115200; });
  REQUIRE_FALSE(wrong_rate.has_value());
  CHECK(wrong_rate.error().code == "serial_reconfigure_failed");
}

TEST_CASE("非白名单速率不持久化也不操作串口") {
  Fixture f;
  bool touched = false;
  const auto result = experiment::ApplyPiBaud(
      f.path, 38400,
      [&](const nlohmann::json&) ->
          std::expected<void, config_command::CommandError> {
        touched = true;
        return {};
      },
      [&] { touched = true; },
      [&](int) -> std::expected<int, std::string> {
        touched = true;
        return 38400;
      });
  REQUIRE_FALSE(result.has_value());
  CHECK_FALSE(touched);
}

TEST_CASE("重启恢复只持久化已核验的基准速率并保留其他配置") {
  Fixture f;
  std::ofstream(f.path) <<
      R"({"serial":{"device":"auto","baud":57600},"mqtt":{"keep":1}})";
  const auto result = experiment::PersistRecoveredBaud(
      f.path, 115200, [&](const nlohmann::json& candidate) {
        return config_command::PersistConfig(
            {.mode = config_command::ConfigWriterMode::kDirect,
             .helper_path = {}},
            f.path, candidate);
      });
  REQUIRE(result.has_value());
  std::ifstream input(f.path);
  const auto persisted = nlohmann::json::parse(input);
  CHECK(persisted.at("serial").at("baud") == 115200);
  CHECK(persisted.at("serial").at("device") == "auto");
  CHECK(persisted.at("mqtt").at("keep") == 1);
}

TEST_CASE("重启恢复不能把未核验或非基准速率写入配置") {
  Fixture f;
  bool called = false;
  const auto result = experiment::PersistRecoveredBaud(
      f.path, 57600,
      [&](const nlohmann::json&) ->
          std::expected<void, config_command::CommandError> {
        called = true;
        return {};
      });
  REQUIRE_FALSE(result.has_value());
  CHECK_FALSE(called);
}

TEST_CASE("恢复重试即使配置已有基准值仍重新确认持久化") {
  Fixture f;
  bool called = false;
  const auto result = experiment::PersistRecoveredBaud(
      f.path, 115200,
      [&](const nlohmann::json& candidate) ->
          std::expected<void, config_command::CommandError> {
        called = true;
        CHECK(candidate.at("serial").at("baud") == 115200);
        return {};
      });
  REQUIRE(result.has_value());
  CHECK(called);
}
