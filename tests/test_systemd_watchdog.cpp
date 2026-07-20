#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "platform/systemd_watchdog.hpp"

using namespace std::chrono_literals;

TEST_CASE("喂狗间隔取 watchdog 超时的 1/3，留两次容错") {
  CHECK(platform::FeedIntervalFor(30'000'000us) == 10'000'000us);
  CHECK(platform::FeedIntervalFor(3'000'000us) == 1'000'000us);
}

TEST_CASE("喂狗定时器首次立即放行，之后按间隔放行") {
  platform::WatchdogFeedTimer timer(10s);
  const auto start = std::chrono::steady_clock::time_point{};

  // 启动后立即喂一次，避免首次喂狗前出现空窗。
  CHECK(timer.ShouldFeed(start));
  CHECK_FALSE(timer.ShouldFeed(start + 1s));
  CHECK_FALSE(timer.ShouldFeed(start + 9s));
  CHECK(timer.ShouldFeed(start + 10s));
  CHECK_FALSE(timer.ShouldFeed(start + 11s));
  CHECK(timer.ShouldFeed(start + 20s));
}

TEST_CASE("未注入 WATCHDOG_USEC 时 watchdog 视为未启用") {
  ::unsetenv("WATCHDOG_USEC");
  ::unsetenv("WATCHDOG_PID");
  CHECK_FALSE(platform::WatchdogTimeout().has_value());
}

TEST_CASE("WATCHDOG_USEC 为 0 或非法值时 watchdog 视为未启用") {
  ::unsetenv("WATCHDOG_PID");

  ::setenv("WATCHDOG_USEC", "0", 1);
  CHECK_FALSE(platform::WatchdogTimeout().has_value());

  ::setenv("WATCHDOG_USEC", "30s", 1);
  CHECK_FALSE(platform::WatchdogTimeout().has_value());

  ::setenv("WATCHDOG_USEC", "", 1);
  CHECK_FALSE(platform::WatchdogTimeout().has_value());

  ::unsetenv("WATCHDOG_USEC");
}

TEST_CASE("WATCHDOG_USEC 合法时返回对应超时") {
  ::unsetenv("WATCHDOG_PID");
  ::setenv("WATCHDOG_USEC", "30000000", 1);

  const auto timeout = platform::WatchdogTimeout();
  REQUIRE(timeout.has_value());
  CHECK(*timeout == 30s);

  ::unsetenv("WATCHDOG_USEC");
}

TEST_CASE("WATCHDOG_PID 不匹配当前进程时不喂别人的狗") {
  ::setenv("WATCHDOG_USEC", "30000000", 1);

  ::setenv("WATCHDOG_PID", std::to_string(::getpid()).c_str(), 1);
  CHECK(platform::WatchdogTimeout().has_value());

  ::setenv("WATCHDOG_PID", std::to_string(::getpid() + 1).c_str(), 1);
  CHECK_FALSE(platform::WatchdogTimeout().has_value());

  ::unsetenv("WATCHDOG_USEC");
  ::unsetenv("WATCHDOG_PID");
}

TEST_CASE("未注入 NOTIFY_SOCKET 时喂狗降级为空操作而不是崩溃") {
  ::unsetenv("NOTIFY_SOCKET");
  CHECK_FALSE(platform::NotifyWatchdogAlive());

  // 路径超过 sun_path 容量时同样安全返回，不写越界。
  ::setenv("NOTIFY_SOCKET", std::string(200, 'x').c_str(), 1);
  CHECK_FALSE(platform::NotifyWatchdogAlive());

  ::unsetenv("NOTIFY_SOCKET");
}
