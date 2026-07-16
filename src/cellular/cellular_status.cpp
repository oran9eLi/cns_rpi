/**
 * @file cellular_status.cpp
 * @brief cellular_status.hpp implementation.
 */

#include "cellular/cellular_status.hpp"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace cellular {

namespace {

constexpr std::int32_t kOnlineBit = 1 << 0;
constexpr std::int32_t kPresentBit = 1 << 1;
constexpr std::int32_t kCarrierBit = 1 << 2;
constexpr std::int32_t kIpBit = 1 << 3;
constexpr std::int32_t kDefaultRouteBit = 1 << 4;

std::string Trim(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.pop_back();
  }
  std::size_t first = 0;
  while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) {
    ++first;
  }
  return text.substr(first);
}

bool ReadFirstLine(const std::filesystem::path& path, std::string* out) {
  std::ifstream input(path);
  if (!input.is_open()) {
    return false;
  }
  std::string line;
  if (!std::getline(input, line)) {
    return false;
  }
  if (out != nullptr) {
    *out = Trim(line);
  }
  return true;
}

bool HasCarrier(std::string_view interface_name) {
  const auto base = std::filesystem::path("/sys/class/net") / std::string(interface_name);
  std::string carrier;
  if (ReadFirstLine(base / "carrier", &carrier)) {
    return carrier == "1";
  }
  std::string operstate;
  if (ReadFirstLine(base / "operstate", &operstate)) {
    return operstate == "up" || operstate == "unknown";
  }
  return false;
}

bool HasIpAddress(std::string_view interface_name) {
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) {
    return false;
  }

  bool found = false;
  for (ifaddrs* current = interfaces; current != nullptr; current = current->ifa_next) {
    if (current->ifa_addr == nullptr || current->ifa_name == nullptr) {
      continue;
    }
    if (interface_name != std::string_view(current->ifa_name)) {
      continue;
    }
    const int family = current->ifa_addr->sa_family;
    if (family == AF_INET || family == AF_INET6) {
      found = true;
      break;
    }
  }

  freeifaddrs(interfaces);
  return found;
}

bool HasIpv4DefaultRoute(std::string_view interface_name) {
  std::ifstream input("/proc/net/route");
  if (!input.is_open()) {
    return false;
  }

  std::string line;
  std::getline(input, line);
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    std::string iface;
    std::string destination;
    fields >> iface >> destination;
    if (std::string_view(iface) == interface_name && destination == "00000000") {
      return true;
    }
  }
  return false;
}

bool HasIpv6DefaultRoute(std::string_view interface_name) {
  std::ifstream input("/proc/net/ipv6_route");
  if (!input.is_open()) {
    return false;
  }

  std::string line;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    std::vector<std::string> parts;
    std::string part;
    while (fields >> part) {
      parts.push_back(part);
    }
    if (parts.size() < 10) {
      continue;
    }
    const bool default_prefix = parts[0] == std::string(32, '0') && parts[1] == "00";
    if (default_prefix && std::string_view(parts.back()) == interface_name) {
      return true;
    }
  }
  return false;
}

bool HasDefaultRoute(std::string_view interface_name) {
  return HasIpv4DefaultRoute(interface_name) || HasIpv6DefaultRoute(interface_name);
}

}  // namespace

LinkStatus ProbeLink(std::string_view interface_name) {
  LinkStatus status;
  const auto iface_path = std::filesystem::path("/sys/class/net") / std::string(interface_name);
  status.interface_present = std::filesystem::exists(iface_path);
  if (!status.interface_present) {
    return status;
  }

  status.carrier_up = HasCarrier(interface_name);
  status.has_ip_address = HasIpAddress(interface_name);
  status.has_default_route = HasDefaultRoute(interface_name);
  status.online = status.interface_present && status.carrier_up && status.has_ip_address &&
                  status.has_default_route;
  return status;
}

std::int32_t PackRpiCellularValue(const LinkStatus& status) {
  std::int32_t value = 0;
  if (status.online) {
    value |= kOnlineBit;
  }
  if (status.interface_present) {
    value |= kPresentBit;
  }
  if (status.carrier_up) {
    value |= kCarrierBit;
  }
  if (status.has_ip_address) {
    value |= kIpBit;
  }
  if (status.has_default_route) {
    value |= kDefaultRouteBit;
  }
  return value;
}

mavlink_message_t BuildRpiCellularHeartbeat(const LinkStatus& status, std::uint8_t system_id,
                                            std::uint8_t component_id,
                                            std::uint32_t time_boot_ms) {
  mavlink_message_t msg{};
  std::array<char, 10> name{};
  const char literal[] = "RPICELL";
  for (std::size_t i = 0; i < sizeof(literal) - 1; ++i) {
    name[i] = literal[i];
  }
  mavlink_msg_named_value_int_pack(system_id, component_id, &msg, time_boot_ms, name.data(),
                                   PackRpiCellularValue(status));
  return msg;
}

}  // namespace cellular
