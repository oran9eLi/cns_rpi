#pragma once

/** @file serial_affinity.hpp @brief 已核验主控箱与物理串口拓扑的最小关联。 */

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace experiment {

struct TrustedSerialEndpoint {
  std::filesystem::path path;
  std::uint8_t system_id{};
  std::uint8_t component_id{};
};

class SerialAffinity {
 public:
  SerialAffinity(std::filesystem::path journal,
                 std::filesystem::path by_path_directory = "/dev/serial/by-path",
                 std::filesystem::path device_directory = "/dev");

  /** @brief 仅由主循环在真实身份核验后调用；本类不会自行推断身份。 */
  std::expected<void, std::string> ObserveVerified(
      std::string_view device_id, const std::filesystem::path& active_node,
      std::uint8_t system_id, std::uint8_t component_id);
  /** @brief 只返回仍能解析到受限 ttyUSB 节点的原 USB 拓扑路径。 */
  std::expected<TrustedSerialEndpoint, std::string> Resolve(
      std::string_view expected_device_id) const;

 private:
  std::expected<std::filesystem::path, std::string> ValidatePath(
      const std::filesystem::path& path) const;
  std::filesystem::path journal_;
  std::filesystem::path by_path_directory_;
  std::filesystem::path device_directory_;
};

}  // namespace experiment
