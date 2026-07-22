#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "cellular/cellular_snapshot.hpp"

namespace {

using namespace std::chrono_literals;

std::filesystem::path WriteSnapshot(std::string_view content) {
  const auto path = std::filesystem::temp_directory_path() / "cns_rpi_cellular_status.json";
  std::ofstream output(path, std::ios::trunc);
  output << content;
  return path;
}

constexpr std::string_view kCompleteSnapshot = R"({
  "present": true,
  "link_state": "DEGRADED",
  "operator": "China Mobile",
  "access_technology": "NR5G-SA",
  "ip_address": "100.77.73.48",
  "rsrp_dbm": -87,
  "rsrq_db": -10,
  "sinr_db": -3,
  "rssi_dbm": -75,
  "tx_bytes": 123,
  "rx_bytes": 456,
  "recover_count": 2,
  "last_recover_at": "2026-07-22T15:30:00.000+08:00",
  "last_error": null,
  "diagnostics": {
    "interface_present": true,
    "carrier_up": true,
    "has_ip_address": true,
    "has_default_route": true
  }
})";

}  // namespace

TEST_CASE("完整5G状态快照可以解析") {
  const auto path = WriteSnapshot(kCompleteSnapshot);
  const auto now = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(path, now);

  const auto snapshot = cellular::ReadStatusSnapshot(path, 30s, now);

  CHECK(snapshot.link_state == cellular::LinkState::kDegraded);
  CHECK(snapshot.present);
  CHECK(snapshot.operator_name == "China Mobile");
  CHECK(snapshot.access_technology == "NR5G-SA");
  CHECK(snapshot.rsrp_dbm == -87);
  CHECK(snapshot.recover_count == 2);
  CHECK(snapshot.diagnostics.has_default_route);
}

TEST_CASE("不存在或损坏的快照返回UNKNOWN且不抛异常") {
  const auto now = std::filesystem::file_time_type::clock::now();
  const auto missing = std::filesystem::temp_directory_path() / "missing_cellular_status.json";
  std::filesystem::remove(missing);

  const auto missing_snapshot = cellular::ReadStatusSnapshot(missing, 30s, now);
  CHECK(missing_snapshot.link_state == cellular::LinkState::kUnknown);
  CHECK(missing_snapshot.last_error == "5G状态快照不存在");

  const auto broken = WriteSnapshot("{broken");
  const auto broken_snapshot = cellular::ReadStatusSnapshot(broken, 30s, now);
  CHECK(broken_snapshot.link_state == cellular::LinkState::kUnknown);
  CHECK(broken_snapshot.last_error == "5G状态快照格式无效");
}

TEST_CASE("过期快照保留观测值并把状态改为UNKNOWN") {
  const auto path = WriteSnapshot(kCompleteSnapshot);
  const auto now = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(path, now - 31s);

  const auto snapshot = cellular::ReadStatusSnapshot(path, 30s, now);

  CHECK(snapshot.link_state == cellular::LinkState::kUnknown);
  CHECK(snapshot.operator_name == "China Mobile");
  CHECK(snapshot.rsrp_dbm == -87);
  CHECK(snapshot.last_error == "5G状态快照已过期");
}

TEST_CASE("公开遥测不包含内部诊断字段") {
  const auto path = WriteSnapshot(kCompleteSnapshot);
  const auto now = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(path, now);
  const auto snapshot = cellular::ReadStatusSnapshot(path, 30s, now);

  const auto json = cellular::BuildPublicTelemetryJson(snapshot);

  CHECK(json["link_state"] == "DEGRADED");
  CHECK(json["operator"] == "China Mobile");
  CHECK(json["last_error"].is_null());
  CHECK_FALSE(json.contains("diagnostics"));
}
