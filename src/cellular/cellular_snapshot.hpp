#pragma once

/**
 * @file cellular_snapshot.hpp
 * @brief 读取 5G 守护服务写入的跨进程状态快照。
 */

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace cellular {

enum class LinkState { kUnknown, kOnline, kDegraded, kOffline, kRecovering };

struct LinkDiagnostics {
  bool interface_present = false;
  bool carrier_up = false;
  bool has_ip_address = false;
  bool has_default_route = false;
};

struct StatusSnapshot {
  bool present = false;
  LinkState link_state = LinkState::kUnknown;
  std::optional<std::string> operator_name;
  std::optional<std::string> access_technology;
  std::optional<std::string> ip_address;
  std::optional<int> rsrp_dbm;
  std::optional<int> rsrq_db;
  std::optional<int> sinr_db;
  std::optional<int> rssi_dbm;
  std::optional<std::uint64_t> tx_bytes;
  std::optional<std::uint64_t> rx_bytes;
  std::optional<double> latency_ms;
  std::optional<double> packet_loss_percent;
  std::uint64_t recover_count = 0;
  std::optional<std::string> last_recover_at;
  std::optional<std::string> last_error;
  LinkDiagnostics diagnostics;
};

StatusSnapshot ReadStatusSnapshot(
    const std::filesystem::path& path, std::chrono::seconds max_age,
    std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now());

nlohmann::json BuildPublicTelemetryJson(const StatusSnapshot& snapshot);

std::string_view LinkStateName(LinkState state);

}  // namespace cellular
