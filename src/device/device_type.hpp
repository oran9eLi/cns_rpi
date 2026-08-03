#pragma once

/**
 * @file device_type.hpp
 * @brief 当前串口会话中的受控设备类型与识别模式。
 *
 * @details
 * 一台树莓派一次只连接一个受控设备。类型决定身份字段语义、命令适配器和
 * MQTT 注册内容；识别模式允许现场在非标准 HEARTBEAT 场景下覆盖自动判断。
 */

#include <string_view>

namespace device {

enum class Type {
  kUnknown,
  kCnsBox,
  kFlightController,
};

enum class DetectionMode {
  kAuto,
  kCnsBox,
  kPx4,
};

constexpr std::string_view TypeName(Type type) {
  switch (type) {
    case Type::kCnsBox:
      return "cns_box";
    case Type::kFlightController:
      return "flight_controller";
    case Type::kUnknown:
      return "unknown";
  }
  return "unknown";
}

constexpr std::string_view DetectionModeName(DetectionMode mode) {
  switch (mode) {
    case DetectionMode::kAuto:
      return "auto";
    case DetectionMode::kCnsBox:
      return "cns_box";
    case DetectionMode::kPx4:
      return "px4";
  }
  return "auto";
}

}  // namespace device
