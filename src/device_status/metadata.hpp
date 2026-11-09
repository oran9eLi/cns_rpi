#pragma once

#include <optional>
#include <string>
#include <vector>

#include "device/capabilities.hpp"
#include "device/product_info.hpp"

namespace device_status {

/// 本地配置提供资料覆盖值；角色号和产品缺省时使用匹配身份的 F407 信息。
struct Metadata {
  std::string device_name{"CNS 一代实训平台"};
  std::string device_type{"xn_box"};
  std::optional<std::string> dcdw_label;
  std::vector<std::string> capabilities{device::kCnsBoxCapabilities.begin(),
                                        device::kCnsBoxCapabilities.end()};
  std::optional<device::ProductInfo> product;
};

}  // namespace device_status
