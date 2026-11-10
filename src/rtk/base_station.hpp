/**
 * @file base_station.hpp
 * @brief RTK 基站实验的独立 USB 接收机端口与实时证据。
 * @details 只能在主循环线程调用；不接触 F407
 * 串口；完整改正帧由主循环取走，不执行固件升级。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace rtk {

/** @brief CRC校验后的完整基站帧，序号贯穿本进程，接收时刻供转发新鲜度判断。 */
struct RtcmFrame {
  std::uint64_t sequence{};
  std::chrono::steady_clock::time_point received_at;
  std::uint16_t message_type{};
  std::vector<std::uint8_t> bytes;
  bool eligible_for_relay{true};
};

/**
 * @brief 已由配置/实验入口校验的参数。
 * @details duration > 0；accuracy_m * 10000 必须能表示为 uint32；stale_after_ms
 * >= 1000。 USB 使用 termios 的 baud 字段打开端点，不修改接收机 UART 波特率。
 */
struct Settings {
  std::string device;
  int baud{115200};
  std::uint32_t survey_min_duration_s{60};
  double survey_accuracy_m{2.0};
  std::uint32_t stale_after_ms{3000};
};

/** @brief 唯一持有基站串口；端口已打开、配置已核实和观测已完成分别报告。 */
class BaseStation {
 public:
  /** @brief 构造不打开串口；Tick 负责连接与只读查询。 */
  explicit BaseStation(Settings settings);
  ~BaseStation();
  BaseStation(const BaseStation &) = delete;
  BaseStation &operator=(const BaseStation &) = delete;
  BaseStation(BaseStation &&) = delete;
  BaseStation &operator=(BaseStation &&) = delete;

  /** @brief 处理当前可读字节和有界配置步骤，now 必须来自同一 steady_clock。 */
  void Tick(std::chrono::steady_clock::time_point now);
  /** @brief 在本轮 Tick 之后排队接受默认参数，完成与否以随后真实 NAV-SVIN
   * 为准。 */
  std::expected<void, std::string> StartSurveyIn();
  /** @brief 仅排队接受入口已校验的本次参数，不持久化接收机配置或修改默认参数。
   */
  std::expected<void, std::string> StartSurveyIn(std::uint32_t min_duration_s,
                                                 double accuracy_m);
  /**
   * @brief 在实验请求过期时取消尚未完成的配置，阻止迟到 ACK 推进下一步。
   * @details 不发送串口写入，不撤销已应用的配置；主循环应在下一次 Tick 前调用。
   */
  void CancelPendingConfiguration();
  /** @brief 返回当前会话证据及各类样本年龄；未知为 null，过期单独标记。 */
  /** @brief 主循环取走待处理帧；队列只保留最新32帧，断连时清空。 */
  std::vector<RtcmFrame> DrainRtcmFrames();
  /// 有界解析并排除授权前的改正；保留UBX/NMEA及跨边界半帧的解析状态。
  void DiscardRtcmBeforeActivation(std::chrono::steady_clock::time_point now);
  nlohmann::json Status(std::chrono::steady_clock::time_point now) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rtk
