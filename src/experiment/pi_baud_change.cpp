/** @file pi_baud_change.cpp @brief 不退出 MQTT 的受限 Pi 串口换速顺序。 */

#include "experiment/pi_baud_change.hpp"

#include "config_command/config_updater.hpp"

namespace experiment {

std::expected<PiBaudApplied, PiBaudFailure> ApplyPiBaud(
    const std::filesystem::path& config_path, int target_baud,
    const BaudPersist& persist, const BaudClose& close,
    const BaudOpen& open) {
  if (target_baud != 57600 && target_baud != 115200) {
    return std::unexpected(PiBaudFailure{
        "invalid_request", "Pi目标波特率不在白名单中", false});
  }
  const auto loaded = config_command::LoadConfigJson(config_path);
  if (!loaded || !loaded->is_object() || !loaded->contains("serial") ||
      !loaded->at("serial").is_object() ||
      !loaded->at("serial").contains("baud")) {
    return std::unexpected(PiBaudFailure{
        "config_write_failed", "无法读取现有串口配置", false});
  }
  auto candidate = *loaded;
  candidate["serial"]["baud"] = target_baud;
  const auto written = persist(candidate);
  if (!written) {
    const auto uncertain = written.error().code == "config_write_uncertain";
    return std::unexpected(PiBaudFailure{
        uncertain ? "config_write_uncertain" : "config_write_failed",
        written.error().message, false});
  }
  close();
  const auto actual = open(target_baud);
  if (!actual || *actual != target_baud) {
    return std::unexpected(PiBaudFailure{
        "serial_reconfigure_failed",
        actual ? "串口实际波特率与目标值不一致" : actual.error(), true});
  }
  return PiBaudApplied{*actual};
}

}  // namespace experiment
