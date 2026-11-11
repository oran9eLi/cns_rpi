#include "device_status/health_tracker.hpp"

#include <algorithm>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace device_status {
namespace {

constexpr HealthClock::duration kFailureLimit = std::chrono::hours(12);
constexpr std::int64_t kFailureLimitMs = 43'200'000;

}  // namespace

HealthTracker::HealthTracker(std::filesystem::path state_file)
    : state_file_(std::move(state_file)) {}

bool HealthTracker::Load(const std::string& device_id, std::string* error) {
  if (error) error->clear();
  device_id_ = device_id;
  failure_time_.fill(HealthClock::duration::zero());
  failing_.fill(false);
  modules_available_ = false;
  last_update_.reset();
  maintenance_ = false;
  if (device_id_.empty()) {
    if (error) *error = "健康状态缺少设备身份";
    return false;
  }

  std::error_code file_error;
  const bool exists = std::filesystem::exists(state_file_, file_error);
  if (file_error) {
    if (error) *error = "无法读取健康状态文件: " + file_error.message();
    return false;
  }
  if (!exists) return true;

  std::ifstream input(state_file_);
  if (!input) {
    if (error) *error = "无法打开健康状态文件";
    return false;
  }
  try {
    nlohmann::json root;
    input >> root;
    if (!root.is_object() || !root.at("schema_version").is_number_integer() ||
        root.at("schema_version") != 1 ||
        !root.at("device_id").is_string() ||
        root.at("device_id").get<std::string>().empty() ||
        !root.at("maintenance").is_boolean() ||
        !root.at("failure_ms").is_array() ||
        root.at("failure_ms").size() != state::kModuleCount) {
      if (error) *error = "健康状态文件字段非法";
      return false;
    }

    std::array<HealthClock::duration, state::kModuleCount> failure_time{};
    const auto& saved = root.at("failure_ms");
    for (std::size_t i = 0; i < failure_time.size(); ++i) {
      if (!saved[i].is_number_integer() || saved[i] < 0 ||
          saved[i] > kFailureLimitMs) {
        if (error) *error = "健康状态文件故障时长非法";
        return false;
      }
      failure_time[i] = std::chrono::milliseconds(saved[i].get<std::int64_t>());
    }
    if (root.at("device_id") == device_id_) {
      failure_time_ = failure_time;
      maintenance_ = root.at("maintenance").get<bool>();
    }
    return true;
  } catch (const nlohmann::json::exception& exception) {
    if (error) *error = "健康状态文件格式错误: " + std::string(exception.what());
    return false;
  }
}

void HealthTracker::Update(
    const std::optional<std::array<std::uint8_t, state::kModuleCount>>& modules,
    HealthClock::time_point now) {
  if (device_id_.empty()) return;
  modules_available_ = modules.has_value();
  for (std::size_t i = 0; i < failure_time_.size(); ++i) {
    if (last_update_ && failing_[i]) {
      failure_time_[i] =
          std::min(kFailureLimit, failure_time_[i] + (now - *last_update_));
    }
    failing_[i] = modules && ((*modules)[i] == 4 || (*modules)[i] == 5);
    if (modules && ((*modules)[i] == 2 || (*modules)[i] == 3 ||
                    (*modules)[i] == 6)) {
      failure_time_[i] = HealthClock::duration::zero();
    }
  }
  last_update_ = now;
}

bool HealthTracker::Save(std::string* error) const {
  if (error) error->clear();
  if (device_id_.empty()) {
    if (error) *error = "健康状态缺少设备身份";
    return false;
  }
  std::array<std::int64_t, state::kModuleCount> failure_ms{};
  for (std::size_t i = 0; i < failure_ms.size(); ++i) {
    failure_ms[i] = std::chrono::duration_cast<std::chrono::milliseconds>(
                        failure_time_[i])
                        .count();
  }
  const nlohmann::json root{{"schema_version", 1},
                            {"device_id", device_id_},
                            {"failure_ms", failure_ms},
                            {"maintenance", maintenance_}};
  std::error_code file_error;
  if (!state_file_.parent_path().empty()) {
    std::filesystem::create_directories(state_file_.parent_path(), file_error);
    if (file_error) {
      if (error) *error = "无法创建健康状态目录: " + file_error.message();
      return false;
    }
  }
  const auto temporary = std::filesystem::path(state_file_.string() + ".tmp");
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      if (error) *error = "无法写入健康状态临时文件";
      return false;
    }
    output << root.dump(2) << '\n';
    output.close();
    if (!output) {
      std::filesystem::remove(temporary, file_error);
      if (error) *error = "写入健康状态临时文件失败";
      return false;
    }
  }
  std::filesystem::rename(temporary, state_file_, file_error);
  if (file_error) {
    const auto message = file_error.message();
    std::filesystem::remove(temporary, file_error);
    if (error) *error = "提交健康状态文件失败: " + message;
    return false;
  }
  return true;
}

bool HealthTracker::SetMaintenance(bool maintenance, std::string* error) {
  const bool previous = maintenance_;
  maintenance_ = maintenance;
  if (Save(error)) return true;
  maintenance_ = previous;
  return false;
}

std::string HealthTracker::Status() const {
  if (maintenance_) return "1";
  if (std::any_of(failure_time_.begin(), failure_time_.end(),
                  [](HealthClock::duration elapsed) {
                    return elapsed >= kFailureLimit;
                  })) {
    return "2";
  }
  return "0";
}

std::optional<std::vector<std::string>> HealthTracker::FaultModules() const {
  std::vector<std::string> modules;
  for (std::size_t i = 0; i < failure_time_.size(); ++i) {
    // 致损进度已落盘；重启或身份暂不可核验时仍需说明损坏原因。
    if (failing_[i] || failure_time_[i] >= kFailureLimit) {
      modules.emplace_back(state::kModuleNames[i]);
    }
  }
  if (!modules_available_ && modules.empty()) return std::nullopt;
  return modules;
}

}  // namespace device_status
