#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "config_command/config_store.hpp"

namespace {
class TempDirectory {
 public:
  TempDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("cns-rpi-store-" + std::to_string(++sequence_));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() { std::filesystem::remove_all(path_); }
  const std::filesystem::path& path() const { return path_; }
 private:
  inline static int sequence_ = 0;
  std::filesystem::path path_;
};

void WriteText(const std::filesystem::path& path, std::string_view text) {
  std::ofstream(path) << text;
}
}  // namespace

TEST_CASE("写入模式默认disabled且helper必须有固定路径") {
  auto disabled = config_command::ParseWriterOptions({});
  REQUIRE(disabled.has_value());
  CHECK(disabled->mode == config_command::ConfigWriterMode::kDisabled);

  auto direct = config_command::ParseWriterOptions({"--config-writer=direct"});
  REQUIRE(direct.has_value());
  CHECK(direct->mode == config_command::ConfigWriterMode::kDirect);
  CHECK_FALSE(config_command::ParseWriterOptions({"--config-writer=helper"}).has_value());
  CHECK_FALSE(config_command::ParseWriterOptions({"--unknown=x"}).has_value());
  CHECK_FALSE(config_command::ParseWriterOptions(
      {"--config-writer=direct", "--config-writer=direct"}).has_value());
}

TEST_CASE("direct原子替换完整JSON且不残留候选文件") {
  TempDirectory directory;
  const auto path = directory.path() / "config.json";
  WriteText(path, R"({"old":true})");
  auto result = config_command::PersistConfig(
      {.mode = config_command::ConfigWriterMode::kDirect, .helper_path = {}}, path,
      nlohmann::json{{"new", true}});
  REQUIRE(result.has_value());
  std::ifstream input(path);
  nlohmann::json actual;
  input >> actual;
  CHECK(actual == nlohmann::json{{"new", true}});
  CHECK(std::distance(std::filesystem::directory_iterator(directory.path()),
                      std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("disabled和写入失败返回稳定错误且保留旧配置") {
  TempDirectory directory;
  const auto path = directory.path() / "config.json";
  WriteText(path, R"({"old":true})");
  auto disabled = config_command::PersistConfig({}, path, nlohmann::json{{"new", true}});
  REQUIRE_FALSE(disabled.has_value());
  CHECK(disabled.error().code == "config_write_disabled");

  auto failed = config_command::PersistConfig(
      {.mode = config_command::ConfigWriterMode::kDirect, .helper_path = {}},
      directory.path() / "missing" / "config.json", nlohmann::json{{"new", true}});
  REQUIRE_FALSE(failed.has_value());
  CHECK(failed.error().code == "config_write_failed");
  std::ifstream input(path);
  nlohmann::json actual;
  input >> actual;
  CHECK(actual == nlohmann::json{{"old", true}});
}

TEST_CASE("helper退出状态决定写入结果") {
  TempDirectory directory;
  const auto path = directory.path() / "config.json";
  WriteText(path, R"({"old":true})");
  auto failed = config_command::PersistConfig(
      {.mode = config_command::ConfigWriterMode::kHelper, .helper_path = "/bin/false"},
      path, nlohmann::json{{"new", true}});
  REQUIRE_FALSE(failed.has_value());
  CHECK(failed.error().code == "config_write_failed");

  auto success = config_command::PersistConfig(
      {.mode = config_command::ConfigWriterMode::kHelper, .helper_path = "/bin/cp"},
      path, nlohmann::json{{"new", true}});
  REQUIRE(success.has_value());
  std::ifstream input(path);
  nlohmann::json actual;
  input >> actual;
  CHECK(actual == nlohmann::json{{"new", true}});
}
