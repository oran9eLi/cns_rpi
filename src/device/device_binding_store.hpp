#pragma once

/**
 * @file device_binding_store.hpp
 * @brief 持久化树莓派与固定主控箱之间最近一次真实验证的身份绑定。
 *
 * @details
 * 本模块只保存绑定身份事实，不保存在线状态、串口端点或 MQTT 状态。已存在的合法
 * 绑定只能核验一致或报告冲突，不能由运行程序自动改绑。
 */

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

#include "device/device_type.hpp"

namespace device {

struct Binding {
  std::string device_id;
  Type device_type{Type::kUnknown};

  bool operator==(const Binding&) const = default;
};

enum class BindOutcome {
  kCreated,   ///< 首次从真实身份原子写入绑定。
  kMatched,   ///< 候选身份与已有绑定一致，文件未改写。
  kConflict,  ///< 候选身份与已有绑定冲突，文件未改写。
};

/**
 * @brief 加载绑定文件。
 * @return 文件不存在返回空 optional；损坏、非法或读取失败返回中文错误。
 */
std::expected<std::optional<Binding>, std::string> LoadBinding(
    const std::filesystem::path& path);

/**
 * @brief 首次原子建绑，或核验候选身份与已有绑定是否一致。
 * @details 本函数不创建父目录；部署脚本负责准备持久目录及权限。
 */
std::expected<BindOutcome, std::string> BindOrVerify(
    const std::filesystem::path& path, const Binding& candidate);

}  // namespace device
