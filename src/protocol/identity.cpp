/**
 * @file identity.cpp
 * @brief identity.hpp 的实现。
 */

#include "protocol/identity.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string_view>

namespace protocol {

std::string FormatDcdwLabel(std::uint8_t sysid) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "DCDW-%03u", static_cast<unsigned>(sysid));
  return std::string(buf);
}

std::optional<std::string> ReadRpiSerial(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::nullopt;
  }
  std::string line;
  while (std::getline(file, line)) {
    constexpr std::string_view kPrefix = "Serial";
    if (line.compare(0, kPrefix.size(), kPrefix) != 0) {
      continue;
    }
    const auto colon_pos = line.find(':');
    if (colon_pos == std::string::npos) {
      continue;
    }
    std::string value = line.substr(colon_pos + 1);
    const auto first = value.find_first_not_of(" \t");
    const auto last = value.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
      return std::nullopt;
    }
    return value.substr(first, last - first + 1);
  }
  return std::nullopt;
}

std::string ExtractVendorId(const std::uint8_t (&uas_id)[20]) {
  const char* data = reinterpret_cast<const char*>(uas_id);
  return std::string(data, strnlen(data, 20));
}

}  // namespace protocol
