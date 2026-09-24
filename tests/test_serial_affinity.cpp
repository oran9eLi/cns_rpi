#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "experiment/serial_affinity.hpp"

namespace {
struct Fixture {
  std::filesystem::path root;
  std::filesystem::path device_root;
  std::filesystem::path by_path;
  std::filesystem::path node;
  std::filesystem::path journal;
  Fixture() {
    static int serial = 0;
    root = std::filesystem::temp_directory_path() /
           ("cns-m1-affinity-" + std::to_string(++serial));
    device_root = root / "dev";
    by_path = device_root / "serial/by-path";
    node = device_root / "ttyUSB0";
    journal = root / "affinity.json";
    std::filesystem::create_directories(by_path);
    std::ofstream(node) << "";
    std::filesystem::create_symlink(node, by_path / "usb-1:2.3");
  }
  ~Fixture() { std::filesystem::remove_all(root); }
};
constexpr auto kId = "DCDWCNS1GHC0G6LF8MY6";
}  // namespace

TEST_CASE("只有真实核验的身份才能建立串口拓扑关联") {
  Fixture f;
  experiment::SerialAffinity affinity(f.journal, f.by_path, f.device_root);
  CHECK_FALSE(affinity.Resolve(kId).has_value());
  REQUIRE(affinity.ObserveVerified(kId, f.node, 42, 193).has_value());
  const auto resolved = affinity.Resolve(kId);
  REQUIRE(resolved.has_value());
  CHECK(resolved->path == f.by_path / "usb-1:2.3");
  CHECK(resolved->system_id == 42);
  CHECK(resolved->component_id == 193);
  CHECK_FALSE(affinity.Resolve("DCDWCNS1OTHERDEVICE").has_value());
}

TEST_CASE("ttyUSB枚举号变动后仍通过同一拓扑路径找到串口") {
  Fixture f;
  experiment::SerialAffinity affinity(f.journal, f.by_path, f.device_root);
  REQUIRE(affinity.ObserveVerified(kId, f.node, 42, 193).has_value());
  std::filesystem::remove(f.by_path / "usb-1:2.3");
  std::filesystem::remove(f.node);
  const auto new_node = f.device_root / "ttyUSB5";
  std::ofstream(new_node) << "";
  std::filesystem::create_symlink(new_node, f.by_path / "usb-1:2.3");
  experiment::SerialAffinity restarted(f.journal, f.by_path, f.device_root);
  REQUIRE(restarted.Resolve(kId).has_value());
  std::filesystem::remove(f.by_path / "usb-1:2.3");
  CHECK_FALSE(restarted.Resolve(kId).has_value());
}

TEST_CASE("不接受不可信节点或被改写到设备目录外的拓扑路径") {
  Fixture f;
  experiment::SerialAffinity affinity(f.journal, f.by_path, f.device_root);
  CHECK_FALSE(affinity.ObserveVerified(kId, f.root / "other", 42, 193).has_value());
  REQUIRE(affinity.ObserveVerified(kId, f.node, 42, 193).has_value());
  std::filesystem::remove(f.by_path / "usb-1:2.3");
  std::filesystem::create_symlink(f.journal, f.by_path / "usb-1:2.3");
  CHECK_FALSE(affinity.Resolve(kId).has_value());
}
