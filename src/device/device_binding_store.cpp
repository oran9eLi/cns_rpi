/**
 * @file device_binding_store.cpp
 * @brief device_binding_store.hpp 的实现。
 */

#include "device/device_binding_store.hpp"

#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "protocol/identity.hpp"

namespace device {

namespace {

constexpr int kBindingSchemaVersion = 1;

bool IsValidBinding(const Binding& binding) {
  return binding.device_type == Type::kCnsBox &&
         protocol::IsValidUasId(binding.device_id);
}

std::string BuildPayload(const Binding& binding) {
  return nlohmann::json{
      {"schema_version", kBindingSchemaVersion},
      {"device_id", binding.device_id},
      {"device_type", "cns_box"},
  }.dump();
}

}  // namespace

std::expected<std::optional<Binding>, std::string> LoadBinding(
    const std::filesystem::path& path) {
  std::error_code exists_error;
  const bool exists = std::filesystem::exists(path, exists_error);
  if (exists_error) {
    return std::unexpected("检查设备绑定文件失败: " + exists_error.message());
  }
  if (!exists) {
    return std::optional<Binding>{};
  }

  std::ifstream input(path);
  if (!input.is_open()) {
    return std::unexpected("打开设备绑定文件失败");
  }

  try {
    nlohmann::json payload;
    input >> payload;
    if (!payload.is_object() ||
        payload.at("schema_version").get<int>() != kBindingSchemaVersion ||
        payload.at("device_type").get<std::string>() != "cns_box") {
      return std::unexpected("设备绑定文件内容非法");
    }
    Binding binding{
        .device_id = payload.at("device_id").get<std::string>(),
        .device_type = Type::kCnsBox,
    };
    if (!IsValidBinding(binding)) {
      return std::unexpected("设备绑定文件身份非法");
    }
    return std::optional<Binding>{std::move(binding)};
  } catch (const nlohmann::json::exception&) {
    return std::unexpected("设备绑定文件不是合法JSON");
  }
}

std::expected<BindOutcome, std::string> BindOrVerify(
    const std::filesystem::path& path, const Binding& candidate) {
  if (!IsValidBinding(candidate)) {
    return std::unexpected("候选设备绑定身份非法");
  }

  auto existing = LoadBinding(path);
  if (!existing) {
    return std::unexpected(existing.error());
  }
  if (*existing) {
    return **existing == candidate ? BindOutcome::kMatched
                                   : BindOutcome::kConflict;
  }

  const std::filesystem::path temporary(path.string() + ".tmp");
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      return std::unexpected("创建设备绑定候选文件失败");
    }
    output << BuildPayload(candidate) << '\n';
    output.close();
    if (!output) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return std::unexpected("写入设备绑定候选文件失败");
    }
  }

  std::error_code rename_error;
  std::filesystem::rename(temporary, path, rename_error);
  if (rename_error) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return std::unexpected("提交设备绑定文件失败: " + rename_error.message());
  }
  return BindOutcome::kCreated;
}

}  // namespace device
