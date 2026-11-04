#pragma once

/**
 * @file device_type.hpp
 * @brief 当前串口会话中的受控主控箱类型。
 */

#include <string_view>

namespace device {

enum class Type {
  kUnknown,
  kCnsBox,
};

constexpr std::string_view TypeName(Type type) {
  switch (type) {
    case Type::kCnsBox:
      return "cns_box";
    case Type::kUnknown:
      return "unknown";
  }
  return "unknown";
}

}  // namespace device
