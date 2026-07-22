/**
 * @file cellular_snapshot.cpp
 * @brief 5G 状态快照的解析、过期处理和公开字段序列化。
 */

#include "cellular/cellular_snapshot.hpp"

#include <fstream>
#include <string_view>

namespace cellular {

namespace {

LinkState ParseLinkState(std::string_view value) {
  if (value == "ONLINE") {
    return LinkState::kOnline;
  }
  if (value == "DEGRADED") {
    return LinkState::kDegraded;
  }
  if (value == "OFFLINE") {
    return LinkState::kOffline;
  }
  if (value == "RECOVERING") {
    return LinkState::kRecovering;
  }
  if (value == "UNKNOWN") {
    return LinkState::kUnknown;
  }
  throw nlohmann::json::type_error::create(302, "link_state取值无效", nullptr);
}

template <typename T>
std::optional<T> ReadOptional(const nlohmann::json& root, std::string_view key) {
  const auto& value = root.at(std::string(key));
  if (value.is_null()) {
    return std::nullopt;
  }
  return value.get<T>();
}

StatusSnapshot ParseSnapshot(const nlohmann::json& root) {
  StatusSnapshot snapshot;
  snapshot.present = root.at("present").get<bool>();
  snapshot.link_state = ParseLinkState(root.at("link_state").get<std::string>());
  snapshot.operator_name = ReadOptional<std::string>(root, "operator");
  snapshot.access_technology = ReadOptional<std::string>(root, "access_technology");
  snapshot.ip_address = ReadOptional<std::string>(root, "ip_address");
  snapshot.rsrp_dbm = ReadOptional<int>(root, "rsrp_dbm");
  snapshot.rsrq_db = ReadOptional<int>(root, "rsrq_db");
  snapshot.sinr_db = ReadOptional<int>(root, "sinr_db");
  snapshot.rssi_dbm = ReadOptional<int>(root, "rssi_dbm");
  snapshot.tx_bytes = ReadOptional<std::uint64_t>(root, "tx_bytes");
  snapshot.rx_bytes = ReadOptional<std::uint64_t>(root, "rx_bytes");
  snapshot.recover_count = root.at("recover_count").get<std::uint64_t>();
  snapshot.last_recover_at = ReadOptional<std::string>(root, "last_recover_at");
  snapshot.last_error = ReadOptional<std::string>(root, "last_error");

  const auto& diagnostics = root.at("diagnostics");
  snapshot.diagnostics.interface_present = diagnostics.at("interface_present").get<bool>();
  snapshot.diagnostics.carrier_up = diagnostics.at("carrier_up").get<bool>();
  snapshot.diagnostics.has_ip_address = diagnostics.at("has_ip_address").get<bool>();
  snapshot.diagnostics.has_default_route = diagnostics.at("has_default_route").get<bool>();
  return snapshot;
}

template <typename T>
void AddOptional(nlohmann::json& out, std::string_view key, const std::optional<T>& value) {
  out[std::string(key)] = value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

StatusSnapshot UnknownSnapshot(std::string error) {
  StatusSnapshot snapshot;
  snapshot.last_error = std::move(error);
  return snapshot;
}

}  // namespace

std::string_view LinkStateName(LinkState state) {
  switch (state) {
    case LinkState::kOnline:
      return "ONLINE";
    case LinkState::kDegraded:
      return "DEGRADED";
    case LinkState::kOffline:
      return "OFFLINE";
    case LinkState::kRecovering:
      return "RECOVERING";
    case LinkState::kUnknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

StatusSnapshot ReadStatusSnapshot(const std::filesystem::path& path,
                                  std::chrono::seconds max_age,
                                  std::filesystem::file_time_type now) {
  std::ifstream input(path);
  if (!input.is_open()) {
    return UnknownSnapshot("5G状态快照不存在");
  }

  try {
    nlohmann::json root;
    input >> root;
    auto snapshot = ParseSnapshot(root);
    const auto modified = std::filesystem::last_write_time(path);
    if (now > modified && now - modified > max_age) {
      snapshot.link_state = LinkState::kUnknown;
      snapshot.last_error = "5G状态快照已过期";
    }
    return snapshot;
  } catch (const nlohmann::json::exception&) {
    return UnknownSnapshot("5G状态快照格式无效");
  } catch (const std::filesystem::filesystem_error&) {
    return UnknownSnapshot("5G状态快照读取失败");
  }
}

nlohmann::json BuildPublicTelemetryJson(const StatusSnapshot& snapshot) {
  nlohmann::json out;
  out["present"] = snapshot.present;
  out["link_state"] = LinkStateName(snapshot.link_state);
  AddOptional(out, "operator", snapshot.operator_name);
  AddOptional(out, "access_technology", snapshot.access_technology);
  AddOptional(out, "ip_address", snapshot.ip_address);
  AddOptional(out, "rsrp_dbm", snapshot.rsrp_dbm);
  AddOptional(out, "rsrq_db", snapshot.rsrq_db);
  AddOptional(out, "sinr_db", snapshot.sinr_db);
  AddOptional(out, "rssi_dbm", snapshot.rssi_dbm);
  AddOptional(out, "tx_bytes", snapshot.tx_bytes);
  AddOptional(out, "rx_bytes", snapshot.rx_bytes);
  out["recover_count"] = snapshot.recover_count;
  AddOptional(out, "last_recover_at", snapshot.last_recover_at);
  AddOptional(out, "last_error", snapshot.last_error);
  return out;
}

}  // namespace cellular
