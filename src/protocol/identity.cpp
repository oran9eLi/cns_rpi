/**
 * @file identity.cpp
 * @brief identity.hpp 的实现。
 */

#include "protocol/identity.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace protocol {

namespace {

/// uas_id 的字节宽度上限，来自 mavlink_open_drone_id_basic_id_t::uas_id。
constexpr std::size_t kUasIdMaxLength = 20;

}  // namespace

std::string FormatDcdwLabel(std::uint8_t sysid) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "DCDW-%03u", static_cast<unsigned>(sysid));
  return std::string(buf);
}

std::string ExtractUasId(const std::uint8_t (&uas_id)[20]) {
  const char* data = reinterpret_cast<const char*>(uas_id);
  return std::string(data, strnlen(data, kUasIdMaxLength));
}

bool IsValidUasId(const std::string& uas_id) {
  return !uas_id.empty() && uas_id.size() <= kUasIdMaxLength &&
         std::all_of(uas_id.begin(), uas_id.end(), [](unsigned char ch) {
           return std::isalnum(ch) != 0 || ch == '-' || ch == '_' ||
                  ch == '.' || ch == ':';
         });
}

bool HasCnsBoxProductPrefix(const std::string& device_id) {
  return device_id.starts_with(std::string(kCnsBoxManufacturerCode) +
                               std::string(kCnsBoxModelCode));
}

std::optional<device::ProductInfo> CnsBoxProductFrom(const std::string& device_id) {
  if (!HasCnsBoxProductPrefix(device_id)) {
    return std::nullopt;
  }
  return device::ProductInfo{
      .manufacturer_code = std::string(kCnsBoxManufacturerCode),
      .model_code = std::string(kCnsBoxModelCode),
  };
}

}  // namespace protocol
