/**
 * @file qgc_udp_bridge.cpp
 * @brief QGC 局域网自动发现、单端点锁定、超时释放和双向 MAVLink 帧转发实现。
 *
 * @details 依赖 Linux IPv4 UDP socket 与 getifaddrs；不创建线程、不保存无限队列，
 * 发送拥塞时直接丢弃当前 UDP 帧，从而把局域网支路故障隔离在 5G/MQTT 主链路之外。
 */

#include "network/qgc_udp_bridge.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <span>
#include <tuple>
#include <utility>

#include "uart/mavlink_link.hpp"

namespace network {

namespace {

constexpr std::size_t kMaxUdpDatagram = 8192;

bool Contains(const std::vector<std::string>& values, const char* value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

std::string_view QgcUdpErrorMessage(QgcUdpError error) {
  switch (error) {
    case QgcUdpError::kSocketOpenFailed:
      return "无法创建QGC UDP套接字";
    case QgcUdpError::kSocketConfigFailed:
      return "无法配置QGC UDP套接字";
    case QgcUdpError::kBindFailed:
      return "无法绑定QGC UDP监听端口";
  }
  return "未知QGC UDP错误";
}

std::expected<QgcUdpBridge, QgcUdpError> QgcUdpBridge::Open(
    Settings settings) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return std::unexpected(QgcUdpError::kSocketOpenFailed);
  }

  const int enabled = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) !=
          0 ||
      ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) !=
          0) {
    ::close(fd);
    return std::unexpected(QgcUdpError::kSocketConfigFailed);
  }

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(settings.listen_port);
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) !=
      0) {
    ::close(fd);
    return std::unexpected(QgcUdpError::kBindFailed);
  }

  socklen_t local_length = sizeof(local);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_length) !=
      0) {
    ::close(fd);
    return std::unexpected(QgcUdpError::kSocketConfigFailed);
  }

  return QgcUdpBridge(fd, std::move(settings), ntohs(local.sin_port));
}

QgcUdpBridge::QgcUdpBridge(int socket_fd, Settings settings,
                           std::uint16_t local_port)
    : socket_fd_(socket_fd),
      settings_(std::move(settings)),
      local_port_(local_port) {}

QgcUdpBridge::QgcUdpBridge(QgcUdpBridge&& other) noexcept
    : socket_fd_(std::exchange(other.socket_fd_, -1)),
      settings_(std::move(other.settings_)),
      local_port_(std::exchange(other.local_port_, 0)),
      lan_interfaces_(std::move(other.lan_interfaces_)),
      peer_(std::move(other.peer_)),
      peer_events_(std::move(other.peer_events_)),
      next_interface_refresh_(other.next_interface_refresh_),
      last_discovery_heartbeat_(other.last_discovery_heartbeat_) {}

QgcUdpBridge& QgcUdpBridge::operator=(QgcUdpBridge&& other) noexcept {
  if (this != &other) {
    if (socket_fd_ >= 0) {
      ::close(socket_fd_);
    }
    socket_fd_ = std::exchange(other.socket_fd_, -1);
    settings_ = std::move(other.settings_);
    local_port_ = std::exchange(other.local_port_, 0);
    lan_interfaces_ = std::move(other.lan_interfaces_);
    peer_ = std::move(other.peer_);
    peer_events_ = std::move(other.peer_events_);
    next_interface_refresh_ = other.next_interface_refresh_;
    last_discovery_heartbeat_ = other.last_discovery_heartbeat_;
  }
  return *this;
}

QgcUdpBridge::~QgcUdpBridge() {
  if (socket_fd_ >= 0) {
    ::close(socket_fd_);
  }
}

void QgcUdpBridge::Maintain(std::chrono::steady_clock::time_point now) {
  if (now >= next_interface_refresh_) {
    RefreshLanInterfaces(now);
  }
  if (peer_ && now - peer_->last_valid_frame >= settings_.peer_timeout) {
    ClearPeer();
  }
}

void QgcUdpBridge::RefreshLanInterfaces(
    std::chrono::steady_clock::time_point now) {
  next_interface_refresh_ = now + settings_.discovery_interval;

  std::vector<LanInterface> refreshed;
  ifaddrs* all_interfaces = nullptr;
  if (::getifaddrs(&all_interfaces) == 0) {
    for (const ifaddrs* item = all_interfaces; item != nullptr;
         item = item->ifa_next) {
      if (item->ifa_addr == nullptr || item->ifa_netmask == nullptr ||
          item->ifa_addr->sa_family != AF_INET ||
          (item->ifa_flags & IFF_UP) == 0 ||
          !Contains(settings_.lan_interfaces, item->ifa_name)) {
        continue;
      }

      const auto* address =
          reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
      const auto* netmask =
          reinterpret_cast<const sockaddr_in*>(item->ifa_netmask);
      std::uint32_t broadcast =
          address->sin_addr.s_addr | ~netmask->sin_addr.s_addr;
      if ((item->ifa_flags & IFF_BROADCAST) != 0 &&
          item->ifa_broadaddr != nullptr) {
        broadcast = reinterpret_cast<const sockaddr_in*>(item->ifa_broadaddr)
                        ->sin_addr.s_addr;
      }
      refreshed.push_back(
          {address->sin_addr.s_addr, netmask->sin_addr.s_addr, broadcast});
    }
    ::freeifaddrs(all_interfaces);
  }

  std::sort(refreshed.begin(), refreshed.end(),
            [](const LanInterface& lhs, const LanInterface& rhs) {
              return std::tie(lhs.address, lhs.netmask, lhs.broadcast) <
                     std::tie(rhs.address, rhs.netmask, rhs.broadcast);
            });
  refreshed.erase(
      std::unique(refreshed.begin(), refreshed.end(),
                  [](const LanInterface& lhs, const LanInterface& rhs) {
                    return lhs.address == rhs.address &&
                           lhs.netmask == rhs.netmask &&
                           lhs.broadcast == rhs.broadcast;
                  }),
      refreshed.end());
  lan_interfaces_ = std::move(refreshed);

  if (peer_ && !IsAllowedPeer(peer_->address)) {
    ClearPeer();
  }
}

bool QgcUdpBridge::IsAllowedPeer(std::uint32_t address) const {
  if (IsOwnAddress(address)) {
    return false;
  }
  return std::any_of(
      lan_interfaces_.begin(), lan_interfaces_.end(),
      [address](const LanInterface& interface) {
        return (address & interface.netmask) ==
               (interface.address & interface.netmask);
      });
}

bool QgcUdpBridge::IsOwnAddress(std::uint32_t address) const {
  return std::any_of(
      lan_interfaces_.begin(), lan_interfaces_.end(),
      [address](const LanInterface& interface) {
        return address == interface.address;
      });
}

bool QgcUdpBridge::IsSelectedPeer(std::uint32_t address,
                                  std::uint16_t port) const {
  return peer_ && peer_->address == address && peer_->port == port;
}

void QgcUdpBridge::SelectPeer(
    std::uint32_t address, std::uint16_t port,
    std::chrono::steady_clock::time_point now) {
  peer_ = Peer{address, port, now};
  peer_events_.push(
      {PeerEventType::kConnected, FormatEndpoint(address, port)});
}

void QgcUdpBridge::SwitchPeer(
    std::uint32_t address, std::uint16_t port,
    std::chrono::steady_clock::time_point now) {
  const std::string previous =
      FormatEndpoint(peer_->address, peer_->port);
  const std::string next = FormatEndpoint(address, port);
  peer_ = Peer{address, port, now};
  peer_events_.push(
      {PeerEventType::kSwitched, previous + " -> " + next});
}

void QgcUdpBridge::ClearPeer() {
  if (!peer_) {
    return;
  }
  peer_events_.push(
      {PeerEventType::kDisconnected,
       FormatEndpoint(peer_->address, peer_->port)});
  peer_.reset();
  last_discovery_heartbeat_ =
      std::chrono::steady_clock::time_point::min();
}

void QgcUdpBridge::SendMessage(const mavlink_message_t& message,
                               std::uint32_t address, std::uint16_t port) {
  std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> bytes{};
  const std::uint16_t length =
      mavlink_msg_to_send_buffer(bytes.data(), &message);
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = address;
  destination.sin_port = htons(port);
  (void)::sendto(socket_fd_, bytes.data(), length, MSG_DONTWAIT | MSG_NOSIGNAL,
                 reinterpret_cast<const sockaddr*>(&destination),
                 sizeof(destination));
}

void QgcUdpBridge::ForwardFlightControllerMessage(
    const mavlink_message_t& message,
    std::chrono::steady_clock::time_point now) {
  Maintain(now);
  if (peer_) {
    SendMessage(message, peer_->address, peer_->port);
    return;
  }

  if (message.msgid != MAVLINK_MSG_ID_HEARTBEAT ||
      (last_discovery_heartbeat_ !=
           std::chrono::steady_clock::time_point::min() &&
       now - last_discovery_heartbeat_ < settings_.discovery_interval)) {
    return;
  }

  last_discovery_heartbeat_ = now;
  for (const auto& interface : lan_interfaces_) {
    SendMessage(message, interface.broadcast, settings_.qgc_port);
  }
}

std::vector<mavlink_message_t> QgcUdpBridge::PollIncoming(
    std::chrono::steady_clock::time_point now) {
  Maintain(now);
  std::vector<mavlink_message_t> accepted;
  std::array<std::uint8_t, kMaxUdpDatagram> bytes{};

  while (true) {
    sockaddr_in sender{};
    socklen_t sender_length = sizeof(sender);
    const ssize_t count = ::recvfrom(
        socket_fd_, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_TRUNC,
        reinterpret_cast<sockaddr*>(&sender), &sender_length);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (count == 0 || static_cast<std::size_t>(count) > bytes.size()) {
      continue;
    }

    const std::uint32_t address = sender.sin_addr.s_addr;
    const std::uint16_t port = ntohs(sender.sin_port);
    if (!IsAllowedPeer(address)) {
      continue;
    }

    uart::MavlinkFrameAssembler assembler;
    std::vector<mavlink_message_t> valid_messages;
    auto message = assembler.Feed(std::span<const std::uint8_t>(
        bytes.data(), static_cast<std::size_t>(count)));
    while (message) {
      valid_messages.push_back(*message);
      message = assembler.Feed(std::span<const std::uint8_t>{});
    }
    if (valid_messages.empty()) {
      continue;
    }

    if (peer_ && !IsSelectedPeer(address, port)) {
      if (now - peer_->last_valid_frame < settings_.handover_idle) {
        continue;
      }
      SwitchPeer(address, port, now);
    } else if (!peer_) {
      SelectPeer(address, port, now);
    } else {
      peer_->last_valid_frame = now;
    }
    if (settings_.allow_commands) {
      accepted.insert(accepted.end(), valid_messages.begin(),
                      valid_messages.end());
    }
  }

  return accepted;
}

std::optional<QgcUdpBridge::PeerEvent> QgcUdpBridge::TakePeerEvent() {
  if (peer_events_.empty()) {
    return std::nullopt;
  }
  auto event = std::move(peer_events_.front());
  peer_events_.pop();
  return event;
}

bool QgcUdpBridge::HasPeer() const { return peer_.has_value(); }

std::uint16_t QgcUdpBridge::LocalPort() const { return local_port_; }

std::string QgcUdpBridge::FormatEndpoint(std::uint32_t address,
                                         std::uint16_t port) {
  in_addr value{};
  value.s_addr = address;
  std::array<char, INET_ADDRSTRLEN> text{};
  if (::inet_ntop(AF_INET, &value, text.data(), text.size()) == nullptr) {
    return "unknown:" + std::to_string(port);
  }
  return std::string(text.data()) + ":" + std::to_string(port);
}

}  // namespace network
