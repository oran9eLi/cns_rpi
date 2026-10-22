/**
 * @file link_lab.cpp
 * @brief LINK_DIAG 四类受限动作与可核对的临时状态事实。
 */

#include "experiment/link_lab.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <expected>
#include <fcntl.h>
#include <fstream>
#include <ranges>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include <nlohmann/json.hpp>

#include "mqtt/topic.hpp"
#include "protocol/identity.hpp"

namespace experiment {
namespace {
using Json = nlohmann::json;
using WallClock = std::chrono::system_clock;
constexpr std::size_t kMaximumPayloadBytes = 4096;
constexpr std::size_t kMaximumActions = 128;
constexpr auto kRetention = std::chrono::seconds(300);

struct Request {
  std::string command_id;
  std::string request_id;
  std::string device_id;
  std::string session_id;
  std::string action_id;
  std::uint64_t lease_version{};
  std::string operation;
  std::string expires_at;
  WallClock::time_point expiry;
  std::optional<std::string> state;
  std::optional<int> baud_rate;
  std::optional<bool> enabled;
};

bool IsUuid(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else if (!((value[i] >= '0' && value[i] <= '9') ||
                 (value[i] >= 'a' && value[i] <= 'f') ||
                 (value[i] >= 'A' && value[i] <= 'F'))) {
      return false;
    }
  }
  return true;
}

bool HasExactKeys(const Json& value, std::initializer_list<std::string_view> keys) {
  return value.is_object() && value.size() == keys.size() &&
         std::ranges::all_of(keys, [&](std::string_view key) {
           return value.contains(std::string(key));
         });
}

std::optional<int> Digits(std::string_view value) {
  int result{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
  return result;
}

std::optional<WallClock::time_point> ParseUtcMillis(std::string_view value) {
  if (value.size() != 24 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value[19] != '.' || value[23] != 'Z') {
    return std::nullopt;
  }
  const auto year = Digits(value.substr(0, 4));
  const auto month = Digits(value.substr(5, 2));
  const auto day = Digits(value.substr(8, 2));
  const auto hour = Digits(value.substr(11, 2));
  const auto minute = Digits(value.substr(14, 2));
  const auto second = Digits(value.substr(17, 2));
  const auto millis = Digits(value.substr(20, 3));
  if (!year || !month || !day || !hour || !minute || !second || !millis ||
      *hour > 23 || *minute > 59 || *second > 59) return std::nullopt;
  const std::chrono::year_month_day date{
      std::chrono::year{*year}, std::chrono::month{static_cast<unsigned>(*month)},
      std::chrono::day{static_cast<unsigned>(*day)}};
  if (!date.ok()) return std::nullopt;
  return std::chrono::sys_days{date} + std::chrono::hours{*hour} +
         std::chrono::minutes{*minute} + std::chrono::seconds{*second} +
         std::chrono::milliseconds{*millis};
}

std::string FormatUtcMillis(WallClock::time_point now) {
  const auto rounded = std::chrono::floor<std::chrono::milliseconds>(now);
  const auto day = std::chrono::floor<std::chrono::days>(rounded);
  const std::chrono::year_month_day date{day};
  const std::chrono::hh_mm_ss time{rounded - day};
  char output[32];
  std::snprintf(output, sizeof(output), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
                static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                static_cast<unsigned>(date.day()),
                static_cast<long long>(time.hours().count()),
                static_cast<long long>(time.minutes().count()),
                static_cast<long long>(time.seconds().count()),
                static_cast<long long>(time.subseconds().count()));
  return output;
}

bool KnownOperation(std::string_view value) {
  return value == "link_lab_set_cellular" || value == "link_lab_set_baud" ||
         value == "link_lab_set_status_reporting" || value == "link_lab_restore";
}

std::expected<Request, std::string> Parse(std::string_view payload) {
  if (payload.size() > kMaximumPayloadBytes) return std::unexpected("链路排障命令超过4096字节");
  try {
    const Json root = Json::parse(payload.begin(), payload.end());
    if (!root.is_object() || !root.contains("operation") ||
        !root.at("operation").is_string()) {
      return std::unexpected("链路排障命令缺少操作");
    }
    const auto operation = root.at("operation").get<std::string>();
    if (!KnownOperation(operation)) return std::unexpected("链路排障操作不支持");
    const bool exact_keys =
        (operation == "link_lab_set_cellular" &&
         HasExactKeys(root, {"schema_version", "command_id", "request_id", "target",
                             "session_id", "action_id", "lease_version", "operation",
                             "expires_at", "state"})) ||
        (operation == "link_lab_set_baud" &&
         HasExactKeys(root, {"schema_version", "command_id", "request_id", "target",
                             "session_id", "action_id", "lease_version", "operation",
                             "expires_at", "baud_rate"})) ||
        (operation == "link_lab_set_status_reporting" &&
         HasExactKeys(root, {"schema_version", "command_id", "request_id", "target",
                             "session_id", "action_id", "lease_version", "operation",
                             "expires_at", "enabled"})) ||
        (operation == "link_lab_restore" &&
         HasExactKeys(root, {"schema_version", "command_id", "request_id", "target",
                             "session_id", "action_id", "lease_version", "operation",
                             "expires_at"}));
    if (!exact_keys || !HasExactKeys(root.at("target"), {"device_id"}) ||
        !root.at("schema_version").is_number_integer() ||
        root.at("schema_version").get<int>() != 1 ||
        !root.at("command_id").is_string() || !root.at("request_id").is_string() ||
        !root.at("target").at("device_id").is_string() ||
        !root.at("session_id").is_string() || !root.at("action_id").is_string() ||
        !root.at("lease_version").is_number_integer() ||
        !root.at("expires_at").is_string()) {
      return std::unexpected("链路排障命令字段或类型非法");
    }
    const auto& lease = root.at("lease_version");
    if ((lease.is_number_unsigned() && lease.get<std::uint64_t>() == 0) ||
        (!lease.is_number_unsigned() && lease.get<std::int64_t>() <= 0)) {
      return std::unexpected("链路排障租约版本非法");
    }
    Request request{.command_id = root.at("command_id").get<std::string>(),
                    .request_id = root.at("request_id").get<std::string>(),
                    .device_id = root.at("target").at("device_id").get<std::string>(),
                    .session_id = root.at("session_id").get<std::string>(),
                    .action_id = root.at("action_id").get<std::string>(),
                    .lease_version = lease.get<std::uint64_t>(),
                    .operation = operation,
                    .expires_at = root.at("expires_at").get<std::string>(),
                    .expiry = {}, .state = std::nullopt,
                    .baud_rate = std::nullopt, .enabled = std::nullopt};
    if (!IsUuid(request.command_id) || !IsUuid(request.request_id) ||
        !IsUuid(request.session_id) || !IsUuid(request.action_id) ||
        request.request_id != request.action_id ||
        !protocol::IsValidUasId(request.device_id)) {
      return std::unexpected("链路排障命令身份非法");
    }
    const auto expiry = ParseUtcMillis(request.expires_at);
    if (!expiry) return std::unexpected("链路排障有效期格式非法");
    request.expiry = *expiry;
    if (operation == "link_lab_set_cellular") {
      if (!root.at("state").is_string()) return std::unexpected("5G教学状态类型非法");
      request.state = root.at("state").get<std::string>();
      if (*request.state != "normal" && *request.state != "disconnected") {
        return std::unexpected("5G教学状态非法");
      }
    } else if (operation == "link_lab_set_baud") {
      if (!root.at("baud_rate").is_number_integer()) {
        return std::unexpected("临时波特率类型非法");
      }
      const auto& baud = root.at("baud_rate");
      if (baud != 57600 && baud != 115200) {
        return std::unexpected("临时波特率仅支持57600或115200");
      }
      request.baud_rate = baud.get<int>();
    } else if (operation == "link_lab_set_status_reporting") {
      if (!root.at("enabled").is_boolean()) return std::unexpected("状态报送开关类型非法");
      request.enabled = root.at("enabled").get<bool>();
    }
    return request;
  } catch (const Json::exception&) {
    return std::unexpected("链路排障命令不是合法JSON或字段类型非法");
  }
}

Json BaseAck(const Request& request, WallClock::time_point now) {
  return {{"schema_version", 1}, {"command_id", request.command_id},
          {"session_id", request.session_id}, {"action_id", request.action_id},
          {"operation", request.operation}, {"observed_at", FormatUtcMillis(now)}};
}

Json Rejected(const Request& request, WallClock::time_point now,
              std::string_view code) {
  auto ack = BaseAck(request, now);
  ack["status"] = "rejected";
  ack["error_code"] = code;
  return ack;
}

std::string Comparison(const Request& request) {
  return Json{{"device_id", request.device_id}, {"session_id", request.session_id},
              {"request_id", request.request_id}, {"lease_version", request.lease_version},
              {"operation", request.operation}, {"expires_at", request.expires_at},
              {"state", request.state ? Json(*request.state) : Json(nullptr)},
              {"baud_rate", request.baud_rate ? Json(*request.baud_rate) : Json(nullptr)},
              {"enabled", request.enabled ? Json(*request.enabled) : Json(nullptr)}}.dump();
}

bool WriteAll(int fd, std::string_view value) {
  while (!value.empty()) {
    const auto written = ::write(fd, value.data(), value.size());
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    value.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}
}  // namespace

bool IsLinkLabOperation(std::string_view payload) {
  if (payload.size() > kMaximumPayloadBytes) return false;
  const auto root = Json::parse(payload.begin(), payload.end(), nullptr, false);
  return root.is_object() && root.contains("operation") &&
         root.at("operation").is_string() &&
         KnownOperation(root.at("operation").get<std::string>());
}

LinkLabController::LinkLabController(std::string topic_namespace,
                                     std::string device_id, int configured_baud,
                                     std::filesystem::path fence_file)
    : topic_namespace_(std::move(topic_namespace)), device_id_(std::move(device_id)),
      configured_baud_(configured_baud), fence_file_(std::move(fence_file)) {
  ready_ = LoadFence();
}

bool LinkLabController::LoadFence() {
  if (fence_file_.empty()) return true;
  std::error_code error;
  if (!std::filesystem::exists(fence_file_, error)) return !error;
  try {
    std::ifstream stream(fence_file_);
    if (!stream) return false;
    const Json saved = Json::parse(stream);
    if (!HasExactKeys(saved, {"schema_version", "device_id", "lease_version", "session_id"}) ||
        saved.at("schema_version") != 1 || saved.at("device_id") != device_id_ ||
        !saved.at("lease_version").is_number_unsigned() ||
        !saved.at("session_id").is_string()) return false;
    lease_version_ = saved.at("lease_version").get<std::uint64_t>();
    session_id_ = saved.at("session_id").get<std::string>();
    previous_process_session_ = true;
    return lease_version_ > 0 && IsUuid(session_id_);
  } catch (const Json::exception&) {
    return false;
  }
}

bool LinkLabController::SaveFence(std::uint64_t lease, std::string_view session) {
  if (fence_file_.empty()) return true;
  const auto payload = Json{{"schema_version", 1}, {"device_id", device_id_},
                            {"lease_version", lease}, {"session_id", session}}.dump();
  const auto temporary = fence_file_.string() + ".tmp";
  const int fd = ::open(temporary.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  const bool stored = WriteAll(fd, payload) && ::fsync(fd) == 0;
  const bool closed = ::close(fd) == 0;
  if (!stored || !closed || ::rename(temporary.c_str(), fence_file_.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return false;
  }
  const auto directory = fence_file_.parent_path().empty()
                             ? std::filesystem::path{"."} : fence_file_.parent_path();
  const int dir_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd < 0) return false;
  const bool synced = ::fsync(dir_fd) == 0;
  ::close(dir_fd);
  return synced;
}

bool LinkLabController::SessionExpired(
    std::chrono::steady_clock::time_point now) const {
  return session_started_at_ && now - *session_started_at_ >=
                                    std::chrono::seconds(300);
}

cellular::LinkStatus LinkLabController::TeachingCellularStatus(
    cellular::LinkStatus actual) const {
  if (cellular_offline_) actual.online = false;
  return actual;
}

void LinkLabController::Abort() {
  temporary_baud_.reset();
  cellular_offline_ = false;
  reporting_enabled_ = true;
  previous_process_session_ = true;
  session_started_at_.reset();
}

Outcome LinkLabController::Handle(std::string_view payload,
                                   const LinkLabActions& actions,
                                   WallClock::time_point wall_now,
                                   std::chrono::steady_clock::time_point steady_now) {
  const auto parsed = Parse(payload);
  if (!parsed) return {.publication = std::nullopt, .diagnostic = parsed.error()};
  const Request& request = *parsed;
  if (request.device_id != device_id_) {
    return {.publication = std::nullopt,
            .diagnostic = "链路排障命令目标与本机绑定身份不符"};
  }
  auto publication = [&](const Json& ack) -> Outcome {
    const auto diagnostic = ack.at("status") == "rejected"
                                ? "链路排障动作拒绝: " +
                                      ack.at("error_code").get<std::string>()
                                : std::string{};
    return {.publication = Publication{
                .topic = mqtt::BuildExperimentAckTopic(topic_namespace_, device_id_),
                .payload = ack.dump(), .qos = 2, .retain = false},
            .diagnostic = diagnostic};
  };
  if (!ready_) {
    return publication(Rejected(request, wall_now, "result_uncertain"));
  }
  if (request.lease_version < lease_version_ ||
      (request.lease_version == lease_version_ && request.session_id != session_id_) ||
      (request.lease_version == lease_version_ && previous_process_session_ &&
       request.operation != "link_lab_restore")) {
    return publication(Rejected(request, wall_now, "action_expired"));
  }
  while (!entries_.empty() && steady_now - entries_.front().completed_at > kRetention) {
    entries_.pop_front();
  }
  const auto comparison = Comparison(request);
  const auto duplicate = std::ranges::find_if(entries_, [&](const Entry& entry) {
    return entry.action_id == request.action_id;
  });
  if (duplicate != entries_.end()) {
    if (duplicate->comparison != comparison) {
      return publication(Rejected(request, wall_now, "duplicate_conflict"));
    }
    auto ack = Json::parse(duplicate->ack);
    ack["command_id"] = request.command_id;
    return publication(ack);
  }

  Json ack;
  if (wall_now >= request.expiry || request.expiry > wall_now + std::chrono::seconds(30) ||
      SessionExpired(steady_now)) {
    ack = Rejected(request, wall_now, "action_expired");
  } else if (request.lease_version > lease_version_ && HasTemporaryOverride()) {
    ack = Rejected(request, wall_now, "result_uncertain");
  } else if (request.lease_version > lease_version_ &&
             !SaveFence(request.lease_version, request.session_id)) {
    ack = Rejected(request, wall_now, "result_uncertain");
  } else {
    if (request.lease_version > lease_version_) {
      lease_version_ = request.lease_version;
      session_id_ = request.session_id;
      previous_process_session_ = false;
      session_started_at_ = steady_now;
    }
    if (request.operation == "link_lab_set_cellular") {
      const bool offline = *request.state == "disconnected";
      const bool sent = (offline || reporting_enabled_) &&
                        actions.send_cellular && actions.send_cellular(offline);
      if (offline && !sent) {
        ack = Rejected(request, wall_now, "serial_unavailable");
      } else {
        cellular_offline_ = offline;
        ack = BaseAck(request, wall_now);
        ack["status"] = "completed";
        ack["fact"] = {{"state", *request.state}, {"report_sent", sent}};
      }
    } else if (request.operation == "link_lab_set_baud") {
      const auto applied = actions.apply_baud ? actions.apply_baud(*request.baud_rate)
                                              : std::nullopt;
      if (!applied) {
        ack = Rejected(request, wall_now, "serial_unavailable");
      } else if (*applied != *request.baud_rate) {
        ack = Rejected(request, wall_now, "result_uncertain");
      } else {
        temporary_baud_ = *applied == configured_baud_ ? std::nullopt : applied;
        ack = BaseAck(request, wall_now);
        ack["status"] = "completed";
        ack["fact"] = {{"baud_rate", *applied}};
      }
    } else if (request.operation == "link_lab_set_status_reporting") {
      reporting_enabled_ = *request.enabled;
      ack = BaseAck(request, wall_now);
      ack["status"] = "completed";
      ack["fact"] = {{"enabled", *request.enabled}};
    } else {
      const auto applied = temporary_baud_
                               ? (actions.apply_baud
                                      ? actions.apply_baud(configured_baud_)
                                      : std::nullopt)
                               : std::optional<int>{configured_baud_};
      if (!applied) {
        ack = Rejected(request, wall_now, "serial_unavailable");
      } else if (*applied != configured_baud_) {
        ack = Rejected(request, wall_now, "result_uncertain");
      } else {
        temporary_baud_.reset();
        cellular_offline_ = false;
        reporting_enabled_ = true;
        ack = BaseAck(request, wall_now);
        ack["status"] = "completed";
        ack["fact"] = {{"restored_baud_rate", configured_baud_},
                       {"cellular_override_cleared", true},
                       {"status_reporting_enabled", true}};
      }
    }
  }
  if (entries_.size() >= kMaximumActions) entries_.pop_front();
  entries_.push_back({.action_id = request.action_id, .comparison = comparison,
                      .ack = ack.dump(), .completed_at = steady_now});
  return publication(ack);
}

}  // namespace experiment
