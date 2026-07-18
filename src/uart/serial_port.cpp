/**
 * @file serial_port.cpp
 * @brief serial_port.hpp 的实现。
 */

#include "uart/serial_port.hpp"

#include <algorithm>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <utility>

namespace uart {

namespace {

/// 把 config.json 里的整数波特率映射成 termios 的 speed_t 常量。
/// 只收 config.example.json 和现有硬件实测会用到的几档，不做穷举——多了也用不上。
std::expected<speed_t, UartError> ToSpeed(int baud) {
  switch (baud) {
    case 9600:
      return B9600;
    case 19200:
      return B19200;
    case 38400:
      return B38400;
    case 57600:
      return B57600;
    case 115200:
      return B115200;
    case 230400:
      return B230400;
    default:
      return std::unexpected(UartError::kConfigFailed);
  }
}

}  // namespace

std::expected<SerialPort, UartError> SerialPort::Open(const std::string& device, int baud) {
  auto speed = ToSpeed(baud);
  if (!speed) {
    return std::unexpected(speed.error());
  }

  int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY);
  if (fd < 0) {
    switch (errno) {
      case ENOENT:
      case ENXIO:
        return std::unexpected(UartError::kDeviceNotFound);
      case EACCES:
      case EPERM:
        return std::unexpected(UartError::kPermissionDenied);
      default:
        return std::unexpected(UartError::kConfigFailed);
    }
  }

  // 主程序和本地测试工具共用这一封装，因此在最底层保证跨进程独占。
  // flock 绑定已打开的文件描述，SerialPort 移动不会释放锁；close 自动释放。
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int lock_error = errno;
    ::close(fd);
    if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
      return std::unexpected(UartError::kDeviceBusy);
    }
    return std::unexpected(UartError::kConfigFailed);
  }

  termios tio{};
  if (::tcgetattr(fd, &tio) != 0) {
    ::close(fd);
    return std::unexpected(UartError::kConfigFailed);
  }

  ::cfmakeraw(&tio);
  ::cfsetispeed(&tio, *speed);
  ::cfsetospeed(&tio, *speed);
  // VMIN=0/VTIME=1：最多等 100ms，没数据也返回，不无限阻塞。
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 1;

  if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
    ::close(fd);
    return std::unexpected(UartError::kConfigFailed);
  }

  return SerialPort(fd);
}

SerialPort::SerialPort(SerialPort&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

SerialPort::~SerialPort() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

std::expected<std::size_t, UartError> SerialPort::Read(std::span<std::uint8_t> buffer) {
  ssize_t n = ::read(fd_, buffer.data(), buffer.size());
  if (n < 0) {
    if (errno == EINTR) {
      return std::size_t{0};
    }
    return std::unexpected(UartError::kReadError);
  }
  return static_cast<std::size_t>(n);
}

std::expected<bool, UartError> SerialPort::WaitReadable(
    std::chrono::milliseconds max_wait) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::max(max_wait, max_wait.zero());

  while (true) {
    const auto remaining = deadline - Clock::now();
    const auto rounded = std::chrono::ceil<std::chrono::milliseconds>(remaining);
    const auto timeout = std::clamp<std::int64_t>(
        rounded.count(), 0, std::numeric_limits<int>::max());
    pollfd descriptor{fd_, POLLIN, 0};
    const int result = ::poll(&descriptor, 1, static_cast<int>(timeout));
    if (result == 0) {
      return false;
    }
    if (result < 0) {
      if (errno == EINTR) {
        if (Clock::now() >= deadline) {
          return false;
        }
        continue;
      }
      return std::unexpected(UartError::kReadError);
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      return std::unexpected(UartError::kReadError);
    }
    return (descriptor.revents & POLLIN) != 0;
  }
}

std::expected<std::size_t, UartError> SerialPort::Write(std::span<const std::uint8_t> data) {
  std::size_t total = 0;
  while (total < data.size()) {
    ssize_t n = ::write(fd_, data.data() + total, data.size() - total);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(UartError::kWriteError);
    }
    total += static_cast<std::size_t>(n);
  }
  return total;
}

}  // namespace uart
