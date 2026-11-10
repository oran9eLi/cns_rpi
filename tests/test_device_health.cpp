#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <vector>

#include "device_status/health_tracker.hpp"

using namespace std::chrono_literals;

namespace {

constexpr auto kStart = device_status::HealthClock::time_point{};
constexpr auto kDeviceId = "DCDWCNS1S2MEAG1VTA0C";
using Modules = std::array<std::uint8_t, state::kModuleCount>;

struct HealthFile {
  HealthFile() {
    static unsigned sequence = 0;
    directory = std::filesystem::temp_directory_path() /
                ("cns-health-" + std::to_string(::getpid()) + "-" +
                 std::to_string(sequence++));
    std::filesystem::create_directories(directory);
  }
  ~HealthFile() { std::filesystem::remove_all(directory); }
  std::filesystem::path Path() const { return directory / "health.json"; }
  std::filesystem::path directory;
};

Modules Failing(std::size_t index = 0) {
  Modules modules{};
  modules.fill(2);
  modules[index] = 4;
  return modules;
}

}  // namespace

TEST_CASE("单模块故障运行满十二小时才报损坏") {
  HealthFile file;
  device_status::HealthTracker health(file.Path());
  std::string error;
  REQUIRE(health.Load(kDeviceId, &error));
  CHECK(error.empty());
  CHECK(health.Status() == "0");
  health.Update(Failing(), kStart);
  health.Update(Failing(), kStart + 12h - 1ms);
  CHECK(health.Status() == "0");
  health.Update(Failing(), kStart + 12h);
  CHECK(health.Status() == "2");
  health.Update(Failing(), kStart + 24h);
  CHECK(health.Status() == "2");
}

TEST_CASE("离线到故障切换仍属于同一次持续失效") {
  HealthFile file;
  device_status::HealthTracker health(file.Path());
  REQUIRE(health.Load(kDeviceId, nullptr));
  auto modules = Failing();
  health.Update(modules, kStart);
  modules[0] = 5;
  health.Update(modules, kStart + 6h);
  health.Update(modules, kStart + 12h);
  CHECK(health.Status() == "2");
}

TEST_CASE("不同模块不能拼接为十二小时故障") {
  HealthFile file;
  device_status::HealthTracker health(file.Path());
  REQUIRE(health.Load(kDeviceId, nullptr));
  health.Update(Failing(0), kStart);
  health.Update(Failing(1), kStart + 6h);
  health.Update(Failing(1), kStart + 12h);
  CHECK(health.Status() == "0");
  health.Update(Failing(1), kStart + 18h);
  CHECK(health.Status() == "2");
}

TEST_CASE("主动关闭和降级不累计并结束原有故障") {
  for (const std::uint8_t state : {2, 3, 6}) {
    CAPTURE(state);
    HealthFile file;
    device_status::HealthTracker health(file.Path());
    REQUIRE(health.Load(kDeviceId, nullptr));
    auto modules = Failing();
    health.Update(modules, kStart);
    health.Update(modules, kStart + 11h);
    modules[0] = state;
    health.Update(modules, kStart + 11h);
    health.Update(modules, kStart + 40h);
    CHECK(health.Status() == "0");
    health.Update(Failing(), kStart + 40h);
    health.Update(Failing(), kStart + 41h);
    CHECK(health.Status() == "0");
    health.Update(Failing(), kStart + 52h);
    CHECK(health.Status() == "2");
  }
}

TEST_CASE("未知和启动状态暂停计时并保留原有进度") {
  for (const std::uint8_t state : {0, 1}) {
    CAPTURE(state);
    HealthFile file;
    device_status::HealthTracker health(file.Path());
    REQUIRE(health.Load(kDeviceId, nullptr));
    auto modules = Failing();
    health.Update(modules, kStart);
    modules[0] = state;
    health.Update(modules, kStart + 6h);
    health.Update(modules, kStart + 30h);
    CHECK(health.Status() == "0");
    health.Update(Failing(), kStart + 30h);
    health.Update(Failing(), kStart + 36h - 1ms);
    CHECK(health.Status() == "0");
    health.Update(Failing(), kStart + 36h);
    CHECK(health.Status() == "2");
  }
}

TEST_CASE("整份模块状态缺失期间不累计故障") {
  HealthFile file;
  device_status::HealthTracker health(file.Path());
  REQUIRE(health.Load(kDeviceId, nullptr));
  health.Update(Failing(), kStart);
  health.Update(std::nullopt, kStart + 6h);
  health.Update(std::nullopt, kStart + 30h);
  CHECK(health.Status() == "0");
  health.Update(Failing(), kStart + 30h);
  health.Update(Failing(), kStart + 36h);
  CHECK(health.Status() == "2");
}

TEST_CASE("恢复后撤销损坏并重新累计新的故障") {
  HealthFile file;
  device_status::HealthTracker health(file.Path());
  REQUIRE(health.Load(kDeviceId, nullptr));
  health.Update(Failing(), kStart);
  health.Update(Failing(), kStart + 12h);
  CHECK(health.Status() == "2");
  Modules online{};
  online.fill(2);
  health.Update(online, kStart + 13h);
  CHECK(health.Status() == "0");
  health.Update(Failing(), kStart + 14h);
  health.Update(Failing(), kStart + 15h);
  CHECK(health.Status() == "0");
}

TEST_CASE("保存后重启续计且不补算关机时间") {
  HealthFile file;
  {
    device_status::HealthTracker health(file.Path());
    REQUIRE(health.Load(kDeviceId, nullptr));
    health.Update(Failing(), kStart);
    health.Update(Failing(), kStart + 6h);
    REQUIRE(health.Save(nullptr));
  }
  device_status::HealthTracker restarted(file.Path());
  REQUIRE(restarted.Load(kDeviceId, nullptr));
  CHECK(restarted.Status() == "0");
  restarted.Update(Failing(), kStart + 200h);
  CHECK(restarted.Status() == "0");
  restarted.Update(Failing(), kStart + 206h - 1ms);
  CHECK(restarted.Status() == "0");
  restarted.Update(Failing(), kStart + 206h);
  CHECK(restarted.Status() == "2");
  REQUIRE(restarted.Save(nullptr));
  nlohmann::json stored;
  std::ifstream(file.Path()) >> stored;
  CHECK(stored.at("failure_ms").at(0) == 43'200'000);
  CHECK(stored.at("device_id") == kDeviceId);
}

TEST_CASE("维修优先并跨重启保持直到显式解除") {
  HealthFile file;
  device_status::HealthTracker health(file.Path());
  REQUIRE(health.Load(kDeviceId, nullptr));
  REQUIRE(health.SetMaintenance(true, nullptr));
  health.Update(Failing(), kStart);
  health.Update(Failing(), kStart + 12h);
  CHECK(health.Status() == "1");
  REQUIRE(health.Save(nullptr));
  device_status::HealthTracker restarted(file.Path());
  REQUIRE(restarted.Load(kDeviceId, nullptr));
  CHECK(restarted.Status() == "1");
  REQUIRE(restarted.SetMaintenance(false, nullptr));
  CHECK(restarted.Status() == "2");
  device_status::HealthTracker reloaded(file.Path());
  REQUIRE(reloaded.Load(kDeviceId, nullptr));
  CHECK(reloaded.Status() == "2");
}

TEST_CASE("切换设备身份不继承其他设备故障和维修状态") {
  HealthFile file;
  device_status::HealthTracker health(file.Path());
  REQUIRE(health.Load(kDeviceId, nullptr));
  health.Update(Failing(), kStart);
  health.Update(Failing(), kStart + 12h);
  REQUIRE(health.SetMaintenance(true, nullptr));
  REQUIRE(health.Load("DCDWCNS1S2MEAG1VTA0D", nullptr));
  CHECK(health.Status() == "0");
  health.Update(Failing(), kStart + 100h);
  CHECK(health.Status() == "0");
  REQUIRE(health.Save(nullptr));
  device_status::HealthTracker reloaded(file.Path());
  REQUIRE(reloaded.Load(kDeviceId, nullptr));
  CHECK(reloaded.Status() == "0");
}

TEST_CASE("没有设备身份不累计也不创建持久文件") {
  HealthFile file;
  device_status::HealthTracker health(file.Path());
  std::string error;
  health.Update(Failing(), kStart);
  health.Update(Failing(), kStart + 12h);
  CHECK(health.Status() == "0");
  CHECK_FALSE(health.Save(&error));
  CHECK_FALSE(error.empty());
  CHECK_FALSE(health.SetMaintenance(true, &error));
  CHECK_FALSE(health.Load("", &error));
  CHECK_FALSE(std::filesystem::exists(file.Path()));
}

TEST_CASE("损坏或类型错误的健康文件返回可见错误") {
  HealthFile file;
  const nlohmann::json valid{{"schema_version", 1}, {"device_id", kDeviceId},
                           {"failure_ms", std::array<std::int64_t, state::kModuleCount>{}},
                           {"maintenance", false}};
  std::vector<nlohmann::json> invalids{nullptr, nlohmann::json::object()};
  auto invalid = valid;
  invalid["failure_ms"][0] = -1;
  invalids.push_back(invalid);
  invalid["failure_ms"][0] = 43'200'001;
  invalids.push_back(invalid);
  invalid = valid;
  invalid["failure_ms"][0] = 0.5;
  invalids.push_back(invalid);
  invalid = valid;
  invalid["failure_ms"] = {0};
  invalids.push_back(invalid);
  invalid = valid;
  invalid["maintenance"] = "false";
  invalids.push_back(invalid);
  invalid = valid;
  invalid["schema_version"] = 2;
  invalids.push_back(invalid);
  for (const auto& root : invalids) {
    CAPTURE(root.dump());
    std::ofstream(file.Path()) << root;
    device_status::HealthTracker health(file.Path());
    std::string error;
    CHECK_FALSE(health.Load(kDeviceId, &error));
    CHECK_FALSE(error.empty());
  }
  std::ofstream(file.Path()) << "not JSON";
  device_status::HealthTracker health(file.Path());
  std::string error;
  CHECK_FALSE(health.Load(kDeviceId, &error));
  CHECK_FALSE(error.empty());
}

TEST_CASE("保存失败不假称维修状态已经持久化") {
  HealthFile file;
  const auto blocked = file.directory / "not-a-directory";
  std::ofstream(blocked) << "blocked";
  device_status::HealthTracker health(blocked / "health.json");
  std::string error;
  REQUIRE(health.Load(kDeviceId, &error));
  CHECK_FALSE(health.Save(&error));
  CHECK_FALSE(error.empty());
  CHECK_FALSE(health.SetMaintenance(true, &error));
  CHECK(health.Status() == "0");
}

TEST_CASE("原子提交失败保留已有状态且清理临时文件") {
  HealthFile file;
  device_status::HealthTracker health(file.Path());
  REQUIRE(health.Load(kDeviceId, nullptr));
  std::filesystem::create_directory(file.Path());
  std::string error;
  CHECK_FALSE(health.Save(&error));
  CHECK_FALSE(error.empty());
  CHECK(std::filesystem::is_directory(file.Path()));
  CHECK_FALSE(std::filesystem::exists(file.Path().string() + ".tmp"));
}
