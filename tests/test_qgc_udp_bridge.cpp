#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "common/mavlink.h"
#include "network/qgc_udp_bridge.hpp"
#include "uart/mavlink_link.hpp"

namespace {

class UdpPeer {
 public:
  explicit UdpPeer(const char* bind_address = "127.0.0.2") {
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    REQUIRE(fd_ >= 0);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    REQUIRE(::inet_pton(AF_INET, bind_address, &local.sin_addr) == 1);
    REQUIRE(::bind(fd_, reinterpret_cast<const sockaddr*>(&local),
                   sizeof(local)) == 0);
  }

  UdpPeer(const UdpPeer&) = delete;
  UdpPeer& operator=(const UdpPeer&) = delete;
  ~UdpPeer() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  void Send(const std::vector<std::uint8_t>& bytes, std::uint16_t port) {
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr) == 1);
    REQUIRE(::sendto(fd_, bytes.data(), bytes.size(), 0,
                     reinterpret_cast<const sockaddr*>(&destination),
                     sizeof(destination)) ==
            static_cast<ssize_t>(bytes.size()));
    // 桥接固定使用 MSG_DONTWAIT；给内核一个调度点，避免慢速 CI/DrvFS 上产生竞态。
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  std::vector<std::uint8_t> Receive() {
    pollfd descriptor{fd_, POLLIN, 0};
    REQUIRE(::poll(&descriptor, 1, 100) == 1);
    std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> buffer{};
    const ssize_t count = ::recv(fd_, buffer.data(), buffer.size(), 0);
    REQUIRE(count > 0);
    return {buffer.begin(), buffer.begin() + count};
  }

  bool HasPending(std::chrono::milliseconds timeout) const {
    pollfd descriptor{fd_, POLLIN, 0};
    return ::poll(&descriptor, 1, static_cast<int>(timeout.count())) == 1;
  }

 private:
  int fd_{-1};
};

mavlink_message_t PackHeartbeat(std::uint8_t system_id,
                                std::uint8_t component_id) {
  mavlink_message_t message{};
  mavlink_msg_heartbeat_pack(
      system_id, component_id, &message, MAV_TYPE_QUADROTOR,
      MAV_AUTOPILOT_PX4, 0, 0, MAV_STATE_ACTIVE);
  return message;
}

std::vector<std::uint8_t> Encode(const mavlink_message_t& message) {
  std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> bytes{};
  const auto length = mavlink_msg_to_send_buffer(bytes.data(), &message);
  return {bytes.begin(), bytes.begin() + length};
}

network::QgcUdpBridge OpenBridge(
    bool allow_commands = true,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
    std::chrono::milliseconds handover_idle =
        std::chrono::milliseconds(2500)) {
  auto bridge = network::QgcUdpBridge::Open(
      {.lan_interfaces = {"lo"},
       .listen_port = 0,
       .qgc_port = 14550,
       .discovery_interval = std::chrono::milliseconds(100),
       .handover_idle = handover_idle,
       .peer_timeout = timeout,
       .allow_commands = allow_commands});
  REQUIRE(bridge.has_value());
  return std::move(*bridge);
}

}  // namespace

TEST_CASE("same-subnet QGC is learned only after a valid MAVLink frame") {
  auto bridge = OpenBridge();
  UdpPeer qgc;
  auto invalid = Encode(PackHeartbeat(255, MAV_COMP_ID_MISSIONPLANNER));
  invalid.back() ^= 0xFF;

  qgc.Send(invalid, bridge.LocalPort());
  CHECK(bridge.PollIncoming(std::chrono::steady_clock::now()).empty());
  CHECK_FALSE(bridge.HasPeer());

  const auto heartbeat = PackHeartbeat(255, MAV_COMP_ID_MISSIONPLANNER);
  qgc.Send(Encode(heartbeat), bridge.LocalPort());
  const auto received =
      bridge.PollIncoming(std::chrono::steady_clock::now());

  REQUIRE(received.size() == 1);
  CHECK(received.front().sysid == 255);
  CHECK(bridge.HasPeer());
  const auto event = bridge.TakePeerEvent();
  REQUIRE(event.has_value());
  CHECK(event->type ==
        network::QgcUdpBridge::PeerEventType::kConnected);
  CHECK(event->endpoint.starts_with("127.0.0.2:"));
}

TEST_CASE("PX4 frames are unicast to the selected QGC peer") {
  auto bridge = OpenBridge();
  UdpPeer qgc;
  qgc.Send(Encode(PackHeartbeat(255, MAV_COMP_ID_MISSIONPLANNER)),
           bridge.LocalPort());
  REQUIRE(bridge.PollIncoming(std::chrono::steady_clock::now()).size() == 1);

  const auto px4_heartbeat = PackHeartbeat(1, MAV_COMP_ID_AUTOPILOT1);
  bridge.ForwardFlightControllerMessage(px4_heartbeat,
                                        std::chrono::steady_clock::now());
  const auto bytes = qgc.Receive();

  uart::MavlinkFrameAssembler assembler;
  const auto decoded = assembler.Feed(bytes);
  REQUIRE(decoded.has_value());
  CHECK(decoded->sysid == 1);
  CHECK(decoded->compid == MAV_COMP_ID_AUTOPILOT1);
}

TEST_CASE("read-only mode learns QGC but never returns downlink commands") {
  auto bridge = OpenBridge(false);
  UdpPeer qgc;
  qgc.Send(Encode(PackHeartbeat(255, MAV_COMP_ID_MISSIONPLANNER)),
           bridge.LocalPort());

  CHECK(bridge.PollIncoming(std::chrono::steady_clock::now()).empty());
  CHECK(bridge.HasPeer());
}

TEST_CASE("inactive QGC peer is released and discovery can resume") {
  const auto timeout = std::chrono::milliseconds(20);
  auto bridge = OpenBridge(true, timeout);
  UdpPeer qgc;
  const auto connected_at = std::chrono::steady_clock::now();
  qgc.Send(Encode(PackHeartbeat(255, MAV_COMP_ID_MISSIONPLANNER)),
           bridge.LocalPort());
  REQUIRE(bridge.PollIncoming(connected_at).size() == 1);
  REQUIRE(bridge.TakePeerEvent().has_value());

  CHECK(bridge.PollIncoming(connected_at + timeout).empty());
  CHECK_FALSE(bridge.HasPeer());
  const auto event = bridge.TakePeerEvent();
  REQUIRE(event.has_value());
  CHECK(event->type ==
        network::QgcUdpBridge::PeerEventType::kDisconnected);
}

TEST_CASE("first valid QGC keeps sole control until timeout then another can win") {
  const auto timeout = std::chrono::milliseconds(20);
  auto bridge = OpenBridge(true, timeout);
  UdpPeer first("127.0.0.2");
  UdpPeer second("127.0.0.3");
  const auto connected_at = std::chrono::steady_clock::now();
  const auto qgc_heartbeat =
      Encode(PackHeartbeat(255, MAV_COMP_ID_MISSIONPLANNER));

  first.Send(qgc_heartbeat, bridge.LocalPort());
  REQUIRE(bridge.PollIncoming(connected_at).size() == 1);
  REQUIRE(bridge.TakePeerEvent().has_value());

  second.Send(qgc_heartbeat, bridge.LocalPort());
  CHECK(bridge.PollIncoming(connected_at + std::chrono::milliseconds(1)).empty());

  bridge.ForwardFlightControllerMessage(
      PackHeartbeat(1, MAV_COMP_ID_AUTOPILOT1),
      connected_at + std::chrono::milliseconds(2));
  CHECK_FALSE(first.Receive().empty());
  CHECK_FALSE(second.HasPending(std::chrono::milliseconds(20)));

  CHECK(bridge.PollIncoming(connected_at + timeout).empty());
  CHECK_FALSE(bridge.HasPeer());
  REQUIRE(bridge.TakePeerEvent().has_value());

  second.Send(qgc_heartbeat, bridge.LocalPort());
  REQUIRE(bridge.PollIncoming(connected_at + timeout +
                              std::chrono::milliseconds(1))
              .size() == 1);
  CHECK(bridge.HasPeer());
  const auto event = bridge.TakePeerEvent();
  REQUIRE(event.has_value());
  CHECK(event->type ==
        network::QgcUdpBridge::PeerEventType::kConnected);
  CHECK(event->endpoint.starts_with("127.0.0.3:"));
}

TEST_CASE("another QGC can take over after selected path becomes briefly idle") {
  const auto handover_idle = std::chrono::milliseconds(20);
  const auto peer_timeout = std::chrono::milliseconds(100);
  auto bridge = OpenBridge(true, peer_timeout, handover_idle);
  UdpPeer first("127.0.0.2");
  UdpPeer second("127.0.0.3");
  const auto connected_at = std::chrono::steady_clock::now();
  const auto qgc_heartbeat =
      Encode(PackHeartbeat(255, MAV_COMP_ID_MISSIONPLANNER));

  first.Send(qgc_heartbeat, bridge.LocalPort());
  REQUIRE(bridge.PollIncoming(connected_at).size() == 1);
  REQUIRE(bridge.TakePeerEvent().has_value());

  second.Send(qgc_heartbeat, bridge.LocalPort());
  CHECK(bridge.PollIncoming(connected_at + handover_idle -
                            std::chrono::milliseconds(1))
            .empty());

  second.Send(qgc_heartbeat, bridge.LocalPort());
  REQUIRE(bridge.PollIncoming(connected_at + handover_idle).size() == 1);
  const auto switched = bridge.TakePeerEvent();
  REQUIRE(switched.has_value());
  CHECK(switched->type == network::QgcUdpBridge::PeerEventType::kSwitched);
  CHECK(switched->endpoint.starts_with("127.0.0.2:"));
  CHECK(switched->endpoint.find(" -> 127.0.0.3:") != std::string::npos);

  bridge.ForwardFlightControllerMessage(
      PackHeartbeat(1, MAV_COMP_ID_AUTOPILOT1),
      connected_at + handover_idle + std::chrono::milliseconds(1));
  CHECK_FALSE(second.Receive().empty());
  CHECK_FALSE(first.HasPending(std::chrono::milliseconds(20)));
}
