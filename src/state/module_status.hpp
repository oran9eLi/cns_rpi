#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace state {

/// 固件 Px4Lite_ModuleId_t 顺序；MODSTAT0 覆盖 0-7，MODSTAT1 覆盖 8-13。
inline constexpr std::size_t kModuleCount = 14;
inline constexpr std::array<std::string_view, kModuleCount> kModuleNames = {
    "GNSS",       "IMU",      "BARO",       "BATTERY", "LORA",    "5G",
    "STORAGE",    "REMOTE_ID", "DISPLAY",   "CONTROL", "ALARM",   "SYSTEM",
    "ESTIMATOR",  "BUSINESS"};

}  // namespace state
