#pragma once

/** @file uart_echo_transaction.hpp
 * @brief H1 单动作状态机；发送前持久化动作身份以阻止崩溃后重发。
 */

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "experiment/pi_link_inspection.hpp"
#include "experiment/uart_echo_request.hpp"
#include "uart/mavlink_link.hpp"

namespace experiment {

struct EchoGate {
  bool serial_open{};
  bool identity_verified{};
  bool cns_box{};
  bool serial_busy{};
  std::uint8_t source_system{};
  std::uint8_t source_component{};
  std::uint8_t target_system{};
  std::uint8_t target_component{};
  int pi_baud{};
};

struct EchoStart {
  std::optional<mavlink_message_t> outbound;
  std::optional<Publication> publication;
  std::string diagnostic;
};

/** @brief 只在主循环访问，保证一个 H1 动作最多发送一次。 */
class UartEchoTransaction {
 public:
  using SteadyClock = std::chrono::steady_clock;
  using WallClock = std::chrono::system_clock;
  using NonceGenerator = std::function<std::expected<std::uint64_t, std::string>()>;

  UartEchoTransaction(std::filesystem::path journal_path, std::string topic_namespace,
                      NonceGenerator nonce_generator = {});

  /** @brief 装载持久化去重记录；失败时调用方必须禁用 H1 发帧。 */
  std::expected<void, std::string> Load();
  EchoStart Start(const UartEchoRequest& request, EchoGate gate,
                  WallClock::time_point wall_now, SteadyClock::time_point steady_now);
  /** @brief 仅在完整写入串口后确认 TX 事实。 */
  std::optional<Publication> OnSent(const uart::SentFrame& sent,
                                    WallClock::time_point wall_now);
  std::optional<Publication> OnFrame(const uart::WireFrame& frame,
                                     WallClock::time_point wall_now);
  std::optional<Publication> OnBusinessFrame(SteadyClock::time_point received_at,
                                             WallClock::time_point wall_now);
  std::optional<Publication> Tick(SteadyClock::time_point now,
                                   WallClock::time_point wall_now);
  std::optional<Publication> Abort(std::string_view error_code,
                                    WallClock::time_point wall_now);
  bool HasPending() const;

 private:
  struct Record {
    std::string action_id;
    std::string comparison;
    std::string terminal_payload;
    WallClock::time_point expiry;
  };
  struct Pending {
    UartEchoRequest request;
    std::string comparison;
    std::uint64_t nonce{};
    EchoGate gate;
    SteadyClock::time_point started_at{};
    SteadyClock::time_point sent_at{};
    bool sent{};
    bool fresh_business_frame{};
    std::vector<std::uint8_t> tx_bytes;
    std::optional<uart::WireFrame> response_frame;
    std::optional<std::uint32_t> f407_baud;
  };

  std::optional<Publication> Finish(std::string_view error_code,
                                    WallClock::time_point wall_now);
  std::expected<void, std::string> SaveRecords() const;
  Publication Publish(const std::string& device_id, const std::string& payload) const;
  std::filesystem::path journal_path_;
  std::string topic_namespace_;
  NonceGenerator nonce_generator_;
  bool loaded_{};
  std::vector<Record> records_;
  std::optional<Pending> pending_;
};

}  // namespace experiment
