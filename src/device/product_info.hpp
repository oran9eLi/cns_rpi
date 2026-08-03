#pragma once

/**
 * @file product_info.hpp
 * @brief 受控设备的产品与版本元数据，跟唯一身份分开建模。
 *
 * @details
 * 见 docs/2026-08-03-主控箱与PX4统一身份数据结构设计.md §4：产品型号和版本号
 * 都不是身份，不进 MQTT topic、不做服务器主键，服务器只从 retained registration
 * 幂等更新它们。所有编码统一用字符串，避免主控箱的文本代码（DCDW/CNS1）和 PX4
 * 的数字代码（26/7）在同一个 JSON 字段上产生类型分叉。
 * 未知字段一律省略，不输出空字符串，也不伪造默认值。
 */

#include <optional>
#include <string>

namespace device {

/// 厂商代码 + 产品型号代码。两者都拿不到时整个结构不应存在（用 optional 包住）。
struct ProductInfo {
  std::string manufacturer_code;
  std::string model_code;
};

/// 硬件与固件版本，两个字段各自可缺失。
struct VersionInfo {
  std::optional<std::string> hardware;
  std::optional<std::string> firmware;
};

}  // namespace device
