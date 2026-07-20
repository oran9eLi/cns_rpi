#pragma once

#include <chrono>
#include <optional>

namespace platform {

/**
 * @brief systemd watchdog 喂狗支持。
 *
 * @details
 * `Restart=always` 只能发现进程退出，发现不了进程还在但主循环卡死的“假死”。
 * 补上 systemd watchdog 后，主循环必须周期性发送 WATCHDOG=1；一旦主循环卡住
 * 停止发送，systemd 会杀掉进程并按 Restart 策略重新拉起。
 *
 * 超时时长不在代码里写死，而是从 systemd 注入的 WATCHDOG_USEC 读取，
 * 这样 unit 文件里的 WatchdogSec= 是唯一事实来源，改 unit 不用同步改代码。
 * 命令行手工运行时没有这些环境变量，整套机制自动降级为空操作。
 */

/// systemd 要求的喂狗超时；未启用 watchdog 时返回空。
/// WATCHDOG_PID 存在且不等于当前进程时也返回空，避免子进程误喂父进程的狗。
std::optional<std::chrono::microseconds> WatchdogTimeout();

/// 向 $NOTIFY_SOCKET 发送一次 WATCHDOG=1。
/// 返回 false 表示未启用 watchdog 或发送失败；调用方无需据此改变控制流。
bool NotifyWatchdogAlive();

/// 按固定间隔放行喂狗；首次立即放行，避免启动后到首次喂狗之间出现空窗。
class WatchdogFeedTimer {
 public:
  using Clock = std::chrono::steady_clock;

  explicit WatchdogFeedTimer(Clock::duration interval) : interval_(interval) {}

  bool ShouldFeed(Clock::time_point now);

 private:
  Clock::duration interval_;
  std::optional<Clock::time_point> last_feed_;
};

/// 由 watchdog 超时推导喂狗间隔：取 1/3，留两次容错。
/// 主循环单次迭代上界在百毫秒量级，远小于该间隔，正常运行不会误触发。
constexpr std::chrono::microseconds FeedIntervalFor(
    std::chrono::microseconds timeout) {
  return timeout / 3;
}

}  // namespace platform
