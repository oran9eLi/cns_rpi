#pragma once

/**
 * @file m1_transaction.hpp
 * @brief M1 受限动作的持久化去重、串口帧关联和终态生成。
 *
 * @details 只在主循环调用；本类不直接拥有串口、配置或 MQTT 连接。
 */

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "experiment/m1_request.hpp"
#include "experiment/pi_link_inspection.hpp"
#include "uart/mavlink_link.hpp"

namespace experiment {

struct M1Gate {
  bool cns_box{};
  bool persisted_binding{};
  bool identity_verified{};
  bool trusted_endpoint{};
  bool serial_open{};
  bool serial_busy{};
  bool mqtt_connected{};
  std::uint8_t source_system{};
  std::uint8_t source_component{};
  std::uint8_t target_system{};
  std::uint8_t target_component{};
  int pi_baud{};
};

struct M1Step {
  std::optional<mavlink_message_t> outbound;
  std::optional<Publication> publication;
};

struct M1Start : M1Step {
  std::optional<int> apply_pi_baud;
  std::string diagnostic;
};

/** @brief 一个物理动作先落盘后执行，重启后不重发不明结果。 */
class M1Transaction {
 public:
  using SteadyClock = std::chrono::steady_clock;
  using WallClock = std::chrono::system_clock;
  using NonceGenerator = std::function<std::expected<std::uint64_t, std::string>()>;
  using DirectorySync = std::function<int(int)>;
  using ForeignActionLookup = std::function<bool(std::string_view)>;

  M1Transaction(std::filesystem::path journal_path, std::string topic_namespace,
                NonceGenerator nonce_generator = {}, DirectorySync directory_sync = {},
                ForeignActionLookup foreign_action_lookup = {});

  /** @brief 读取持久化动作记录；失败后禁止任何 M1 物理副作用。 */
  std::expected<void, std::string> Load();
  /** @brief 查询另一种实验操作是否已占用动作 ID。 */
  bool HasActionId(std::string_view action_id) const;
  /** @brief 重启后补发已保存的原始终态，不重新执行动作。 */
  std::vector<Publication> RecoveredPublications() const;
  /** @brief 返回并清除最近一次持久化诊断。 */
  std::string TakeDiagnostic();

  M1Start Start(const M1Request& request, M1Gate gate,
                WallClock::time_point wall_now, SteadyClock::time_point steady_now);
  M1Step OnSent(const uart::SentFrame& sent, WallClock::time_point wall_now);
  M1Step OnFrame(const uart::WireFrame& frame, WallClock::time_point wall_now);
  M1Step Tick(SteadyClock::time_point now, WallClock::time_point wall_now);
  M1Step Abort(std::string_view error_code, WallClock::time_point wall_now);

  /** @brief 配置已持久化且串口重新打开并读回目标值后形成成功事实。 */
  std::optional<Publication> OnPiBaudApplied(int applied_baud,
                                              bool serial_port_open,
                                              WallClock::time_point wall_now);
  /** @brief 本地换速失败时保存明确拒绝终态。 */
  std::optional<Publication> OnPiBaudFailed(std::string_view error_code,
                                             WallClock::time_point wall_now);
  bool HasPending() const;

 private:
  struct Record {
    std::string action_id;
    std::string comparison;
    std::string terminal_payload;
    WallClock::time_point expiry;
  };
  struct ProbeSample {
    std::uint64_t nonce{};
    std::vector<std::uint8_t> tx_bytes;
    std::optional<uart::WireFrame> response;
    std::optional<std::int64_t> round_trip_us;
  };
  struct Pending {
    M1Request request;
    M1Gate gate;
    std::uint64_t nonce{};
    SteadyClock::time_point sent_at{};
    SteadyClock::time_point next_send_at{};
    bool sent{};
    std::vector<std::uint8_t> tx_bytes;
    std::optional<uart::WireFrame> acceptance_frame;
    std::optional<int> applied_pi_baud;
    std::vector<ProbeSample> samples;
  };

  std::expected<void, std::string> SaveRecords();
  Publication Publish(const std::string& device_id, const std::string& payload) const;
  M1Step Finish(std::string_view error_code, WallClock::time_point wall_now);
  M1Step AdvanceProbe(SteadyClock::time_point now, WallClock::time_point wall_now);
  std::filesystem::path journal_path_;
  std::string topic_namespace_;
  NonceGenerator nonce_generator_;
  DirectorySync directory_sync_;
  ForeignActionLookup foreign_action_lookup_;
  bool loaded_{};
  bool last_save_renamed_{};
  bool storage_ambiguous_{};
  std::string storage_diagnostic_;
  std::vector<Record> records_;
  std::optional<Pending> pending_;
};

}  // namespace experiment
