#pragma once

/**
 * @file qgc_udp_bridge.hpp
 * @brief PX4 串口会话与获准网络上的 QGroundControl 之间的非阻塞 MAVLink UDP 桥接。
 *
 * @details
 * 本模块不打开飞控串口、不解析业务字段、不接触 StateStore 或 MQTT。主循环把已通过
 * CRC 校验的飞控帧交给它发送，并把它返回的 QGC 帧写入现有串口。仅从配置指定的
 * 网络接口计算子网与定向广播。可同时配置 wlan0 和 WireGuard 的 wg0，但不会把
 * 5G 公网接口 usb0 隐式纳入控制面。
 */

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

#include "common/mavlink.h"

namespace network {

enum class QgcUdpError {
  kSocketOpenFailed,
  kSocketConfigFailed,
  kBindFailed,
};

std::string_view QgcUdpErrorMessage(QgcUdpError error);

/**
 * @brief 在 cns_rpi 独占的 PX4 串口会话与一个获准 QGC 端点之间转发合法帧。
 * @details 未发现端点时只定时广播 PX4 HEARTBEAT；第一个返回合法 MAVLink 帧的
 * 端点获得唯一控制权；当前端持续通信时其他端点被忽略，当前端进入短暂空闲后允许
 * 新端点以合法帧快速接管，完全超时后释放。所有 socket 操作均非阻塞，UDP 故障
 * 不得阻塞串口或 MQTT 主链路。本类只在 cns_rpi 主线程调用。
 */
class QgcUdpBridge {
 public:
  struct Settings {
    std::vector<std::string> lan_interfaces;
    std::uint16_t listen_port{14540};
    std::uint16_t qgc_port{14550};
    std::chrono::milliseconds discovery_interval{1000};
    std::chrono::milliseconds handover_idle{2500};
    std::chrono::milliseconds peer_timeout{5000};
    bool allow_commands{true};
  };

  enum class PeerEventType {
    kConnected,
    kDisconnected,
    kSwitched,
  };

  struct PeerEvent {
    PeerEventType type;
    std::string endpoint;
  };

  static std::expected<QgcUdpBridge, QgcUdpError> Open(Settings settings);

  QgcUdpBridge(QgcUdpBridge&& other) noexcept;
  QgcUdpBridge& operator=(QgcUdpBridge&& other) noexcept;
  QgcUdpBridge(const QgcUdpBridge&) = delete;
  QgcUdpBridge& operator=(const QgcUdpBridge&) = delete;
  ~QgcUdpBridge();

  /// @brief 向已选 QGC 单播一帧；尚未选择端点时仅按配置周期广播 HEARTBEAT。
  void ForwardFlightControllerMessage(
      const mavlink_message_t& message,
      std::chrono::steady_clock::time_point now);

  /// @brief 非阻塞排空当前 UDP 数据报，只返回当前控制端的合法 MAVLink 帧。
  /// @details `allow_commands=false` 时仍可学习端点，但返回空数组，保证只读观察。
  std::vector<mavlink_message_t> PollIncoming(
      std::chrono::steady_clock::time_point now);

  std::optional<PeerEvent> TakePeerEvent();
  bool HasPeer() const;

  /// @brief 返回实际监听端口；仅单元测试传入 0 时可能不同于配置值。
  std::uint16_t LocalPort() const;

 private:
  struct LanInterface {
    std::uint32_t address;
    std::uint32_t netmask;
    std::uint32_t broadcast;
  };

  struct Peer {
    std::uint32_t address;
    std::uint16_t port;
    std::chrono::steady_clock::time_point last_valid_frame;
  };

  QgcUdpBridge(int socket_fd, Settings settings, std::uint16_t local_port);

  void Maintain(std::chrono::steady_clock::time_point now);
  void RefreshLanInterfaces(std::chrono::steady_clock::time_point now);
  bool IsAllowedPeer(std::uint32_t address) const;
  bool IsOwnAddress(std::uint32_t address) const;
  bool IsSelectedPeer(std::uint32_t address, std::uint16_t port) const;
  void SelectPeer(std::uint32_t address, std::uint16_t port,
                  std::chrono::steady_clock::time_point now);
  void SwitchPeer(std::uint32_t address, std::uint16_t port,
                  std::chrono::steady_clock::time_point now);
  void ClearPeer();
  void SendMessage(const mavlink_message_t& message, std::uint32_t address,
                   std::uint16_t port);
  static std::string FormatEndpoint(std::uint32_t address, std::uint16_t port);

  int socket_fd_{-1};
  Settings settings_;
  std::uint16_t local_port_{0};
  std::vector<LanInterface> lan_interfaces_;
  std::optional<Peer> peer_;
  std::queue<PeerEvent> peer_events_;
  std::chrono::steady_clock::time_point next_interface_refresh_{
      std::chrono::steady_clock::time_point::min()};
  std::chrono::steady_clock::time_point last_discovery_heartbeat_{
      std::chrono::steady_clock::time_point::min()};
};

}  // namespace network
