#include "platform/systemd_watchdog.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace platform {
namespace {

/// 读取环境变量并解析为无符号整数；缺失或格式非法时返回空。
std::optional<unsigned long long> ReadUnsignedEnv(const char* name) {
  const char* raw = std::getenv(name);
  if (raw == nullptr) {
    return std::nullopt;
  }
  const std::string_view text(raw);
  unsigned long long value = 0;
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(text.data(), end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

std::optional<std::chrono::microseconds> WatchdogTimeout() {
  const auto usec = ReadUnsignedEnv("WATCHDOG_USEC");
  if (!usec || *usec == 0) {
    return std::nullopt;
  }
  // WATCHDOG_PID 可选；一旦存在就必须匹配当前进程，否则这条狗不归我们喂。
  if (const auto pid = ReadUnsignedEnv("WATCHDOG_PID")) {
    if (*pid != static_cast<unsigned long long>(::getpid())) {
      return std::nullopt;
    }
  }
  return std::chrono::microseconds(static_cast<std::int64_t>(*usec));
}

bool NotifyWatchdogAlive() {
  const char* socket_path = std::getenv("NOTIFY_SOCKET");
  if (socket_path == nullptr || *socket_path == '\0') {
    return false;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::size_t path_length = std::strlen(socket_path);
  // sun_path 需留出结尾，抽象命名空间下首字节为 '\0' 也占位。
  if (path_length >= sizeof(address.sun_path)) {
    return false;
  }
  std::memcpy(address.sun_path, socket_path, path_length);
  // systemd 用 '@' 表示 Linux 抽象命名空间套接字，实际首字节应为 '\0'。
  if (address.sun_path[0] == '@') {
    address.sun_path[0] = '\0';
  }

  const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return false;
  }

  static constexpr std::string_view kMessage = "WATCHDOG=1";
  const auto address_size = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + path_length);
  ssize_t sent = 0;
  do {
    sent = ::sendto(fd, kMessage.data(), kMessage.size(), MSG_NOSIGNAL,
                    reinterpret_cast<const sockaddr*>(&address), address_size);
  } while (sent < 0 && errno == EINTR);

  ::close(fd);
  return sent == static_cast<ssize_t>(kMessage.size());
}

bool WatchdogFeedTimer::ShouldFeed(Clock::time_point now) {
  if (last_feed_ && now - *last_feed_ < interval_) {
    return false;
  }
  last_feed_ = now;
  return true;
}

}  // namespace platform
