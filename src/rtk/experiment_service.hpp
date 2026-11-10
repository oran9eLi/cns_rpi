#pragma once

/**
 * @file experiment_service.hpp
 * @brief 箱内独立RTK实验链路；不持有MQTT，也不改变F407业务身份。
 */

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace config { struct RtkBaseConfig; }
namespace logging { class Logger; }

namespace rtk {

/**
 * @brief 主循环线程使用的基站与PX4附属观测服务，所有方法须串行调用。
 * @details 配置入口已确认三个不同设备：F407由原主流程持有，本服务只持有
 * rtk_base.device及px4_device。构造不打开端口；PX4缺失不阻断基站实验。
 */
class ExperimentService {
 public:
  using Clock = std::chrono::steady_clock;

  /// 复制已验证的RTK配置；logger须覆盖服务生命周期，不读取F407绑定。
  ExperimentService(const config::RtkBaseConfig& config, logging::Logger& logger);
  ~ExperimentService();
  ExperimentService(const ExperimentService&) = delete;
  ExperimentService& operator=(const ExperimentService&) = delete;

  /// 每轮零等待读取有界可用数据，更新基站和PX4链路，并定时重连。
  void Tick(Clock::time_point now);
  /// 直接提供接收机状态，供已有RTK动作控制器的Sampler使用。
  [[nodiscard]] nlohmann::json BaseStatus(Clock::time_point now) const;
  /// 返回{base,px4}；PX4身份与遥测仅在px4子对象内，不含箱子顶层身份。
  [[nodiscard]] nlohmann::json Status(Clock::time_point now,
                                     bool include_samples = true) const;
  /// 仅改变本地改正转发状态；开始/恢复可以进入等待，不意味着接收机已使用改正。
  std::expected<void, std::string> ControlRelay(
      std::string_view operation, Clock::time_point now = Clock::now());
  /// 入口已校验的参数直接交给基站；不要求PX4在线或已取得Basic ID。
  std::expected<void, std::string> StartSurveyIn(std::uint32_t duration_s,
                                                double accuracy_m);
  /// 主循环在配置动作过期/失去授权时、下一轮Tick前调用；不撤销已应用参数。
  void CancelPendingConfiguration();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rtk
