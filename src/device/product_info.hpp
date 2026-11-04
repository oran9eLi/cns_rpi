#pragma once

/**
 * @file product_info.hpp
 * @brief F407 主控箱产品元数据，与设备唯一身份分开建模。
 *
 * @details
 * 产品代码从主控箱设备编号的 DCDWCNS1 前缀提取，只随 retained registration
 * 上报，不作为 MQTT topic 或服务器主键。
 * 未知字段一律省略，不输出空字符串，也不伪造默认值。
 */

#include <string>

namespace device {

/// 厂商代码 + 产品型号代码。两者都拿不到时整个结构不应存在（用 optional 包住）。
struct ProductInfo {
  std::string manufacturer_code;
  std::string model_code;
};

}  // namespace device
