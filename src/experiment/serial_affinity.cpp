/** @file serial_affinity.cpp @brief 持久化已核验的主控箱串口拓扑。 */

#include "experiment/serial_affinity.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>

#include "protocol/identity.hpp"

namespace experiment {
namespace {
using Json = nlohmann::json;

bool WriteAll(int fd, std::string_view content) {
  while (!content.empty()) {
    const auto count = ::write(fd, content.data(), content.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    content.remove_prefix(static_cast<std::size_t>(count));
  }
  return true;
}
}  // namespace

SerialAffinity::SerialAffinity(std::filesystem::path journal,
                               std::filesystem::path by_path_directory,
                               std::filesystem::path device_directory)
    : journal_(std::move(journal)),
      by_path_directory_(std::move(by_path_directory)),
      device_directory_(std::move(device_directory)) {}

std::expected<std::filesystem::path, std::string> SerialAffinity::ValidatePath(
    const std::filesystem::path& path) const {
  if (path.parent_path() != by_path_directory_) {
    return std::unexpected("串口拓扑路径不属于受信目录");
  }
  std::error_code error;
  if (!std::filesystem::is_symlink(path, error) || error) {
    return std::unexpected("串口拓扑符号链接不存在");
  }
  const auto resolved = std::filesystem::canonical(path, error);
  if (error || resolved.parent_path() != device_directory_ ||
      !resolved.filename().string().starts_with("ttyUSB")) {
    return std::unexpected("串口拓扑路径未指向预期的USB串口节点");
  }
  return resolved;
}

std::expected<void, std::string> SerialAffinity::ObserveVerified(
    std::string_view device_id, const std::filesystem::path& active_node,
    std::uint8_t system_id, std::uint8_t component_id) {
  if (!protocol::IsValidUasId(std::string{device_id}) ||
      !device_id.starts_with("DCDWCNS1") ||
      system_id == 0 || component_id == 0) {
    return std::unexpected("主控箱身份或MAVLink端点非法");
  }
  std::error_code error;
  const auto active = std::filesystem::canonical(active_node, error);
  if (error || active.parent_path() != device_directory_ ||
      !active.filename().string().starts_with("ttyUSB")) {
    return std::unexpected("当前串口不是受信的USB节点");
  }
  std::vector<std::filesystem::path> matches;
  for (std::filesystem::directory_iterator it(by_path_directory_, error), end;
       !error && it != end; it.increment(error)) {
    const auto candidate = it->path();
    const auto resolved = ValidatePath(candidate);
    if (resolved && *resolved == active) matches.push_back(candidate);
  }
  if (error || matches.empty()) {
    return std::unexpected("当前串口没有可核对的USB拓扑路径");
  }
  std::sort(matches.begin(), matches.end());
  // 同一个 ttyUSB 节点可能同时有 usb 与 usbv2 别名；只保留稳定排序的
  // 第一条。所有候选均已解析到同一当前节点，不把不同设备混为一体。
  const auto content = Json{{"schema_version", 1},
                            {"device_id", device_id},
                            {"path", matches.front().string()},
                            {"system_id", system_id},
                            {"component_id", component_id}}.dump();
  const auto temporary = journal_.string() + ".tmp";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return std::unexpected("创建串口拓扑记录失败");
  const bool saved = WriteAll(fd, content) && ::fsync(fd) == 0;
  const bool closed = ::close(fd) == 0;
  if (!saved || !closed || ::rename(temporary.c_str(), journal_.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return std::unexpected("持久化串口拓扑记录失败");
  }
  const auto parent = journal_.parent_path().empty() ? std::filesystem::path{"."}
                                                      : journal_.parent_path();
  const int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd < 0) return std::unexpected("同步串口拓扑记录目录失败");
  const bool synced = ::fsync(dir_fd) == 0;
  ::close(dir_fd);
  if (!synced) return std::unexpected("同步串口拓扑记录目录失败");
  return {};
}

std::expected<TrustedSerialEndpoint, std::string> SerialAffinity::Resolve(
    std::string_view expected_device_id) const {
  std::ifstream input(journal_, std::ios::binary);
  if (!input) return std::unexpected("尚无已核验串口拓扑记录");
  try {
    const auto record = Json::parse(input);
    if (!record.is_object() || record.at("schema_version") != 1 ||
        record.at("device_id") != expected_device_id) {
      return std::unexpected("串口拓扑记录的设备身份不匹配");
    }
    const auto system = record.at("system_id").get<int>();
    const auto component = record.at("component_id").get<int>();
    if (system < 1 || system > 255 || component < 1 || component > 255) {
      return std::unexpected("串口拓扑记录的MAVLink端点非法");
    }
    const std::filesystem::path path{record.at("path").get<std::string>()};
    if (const auto resolved = ValidatePath(path); !resolved) {
      return std::unexpected(resolved.error());
    }
    return TrustedSerialEndpoint{path, static_cast<std::uint8_t>(system),
                                 static_cast<std::uint8_t>(component)};
  } catch (const Json::exception&) {
    return std::unexpected("串口拓扑记录格式非法");
  }
}

}  // namespace experiment
