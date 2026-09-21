#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "device/device_binding_store.hpp"

namespace {

class TempDirectory {
 public:
  TempDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("cns-rpi-binding-test-" +
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count()));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() { std::filesystem::remove_all(path_); }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

device::Binding ValidBinding() {
  return {
      .device_id = "DCDWCNS1GHC0G6LF8MY6",
      .device_type = device::Type::kCnsBox,
  };
}

void WriteText(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::trunc);
  REQUIRE(out.is_open());
  out << content;
}

}  // namespace

TEST_CASE("绑定文件不存在时返回未绑定而不是伪造身份") {
  TempDirectory temp;
  const auto loaded =
      device::LoadBinding(temp.path() / "device_binding.json");

  REQUIRE(loaded.has_value());
  CHECK_FALSE(loaded->has_value());
}

TEST_CASE("首次绑定原子写入后可重新加载") {
  TempDirectory temp;
  const auto path = temp.path() / "device_binding.json";

  const auto outcome = device::BindOrVerify(path, ValidBinding());
  REQUIRE(outcome.has_value());
  CHECK(*outcome == device::BindOutcome::kCreated);

  const auto loaded = device::LoadBinding(path);
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->has_value());
  CHECK(**loaded == ValidBinding());
  CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
}

TEST_CASE("相同身份只验证而冲突身份不覆盖已有绑定") {
  TempDirectory temp;
  const auto path = temp.path() / "device_binding.json";
  REQUIRE(device::BindOrVerify(path, ValidBinding()).has_value());

  const auto matched = device::BindOrVerify(path, ValidBinding());
  REQUIRE(matched.has_value());
  CHECK(*matched == device::BindOutcome::kMatched);

  auto conflict_binding = ValidBinding();
  conflict_binding.device_id = "DCDWCNS1OTHERDEVICE";
  const auto conflict = device::BindOrVerify(path, conflict_binding);
  REQUIRE(conflict.has_value());
  CHECK(*conflict == device::BindOutcome::kConflict);

  const auto loaded = device::LoadBinding(path);
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->has_value());
  CHECK((**loaded).device_id == "DCDWCNS1GHC0G6LF8MY6");
}

TEST_CASE("损坏或不合法的绑定文件返回错误") {
  TempDirectory temp;
  const auto path = temp.path() / "device_binding.json";

  for (const auto& content : {
           std::string{"{bad json"},
           std::string{R"({"schema_version":2,"device_id":"DCDWCNS1GHC0G6LF8MY6","device_type":"cns_box"})"},
           std::string{R"({"schema_version":1,"device_id":"bad/id","device_type":"cns_box"})"},
           std::string{R"({"schema_version":1,"device_id":"PX4-DEVICE","device_type":"flight_controller"})"},
       }) {
    WriteText(path, content);
    const auto loaded = device::LoadBinding(path);
    CHECK_FALSE(loaded.has_value());
  }
}

TEST_CASE("写入失败不留下正式文件或半截候选文件") {
  TempDirectory temp;
  const auto path = temp.path() / "missing" / "device_binding.json";

  const auto outcome = device::BindOrVerify(path, ValidBinding());

  CHECK_FALSE(outcome.has_value());
  CHECK_FALSE(std::filesystem::exists(path));
  CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
}
