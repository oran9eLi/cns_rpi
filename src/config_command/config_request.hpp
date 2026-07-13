#pragma once

/**
 * @file config_request.hpp
 * @brief 构造设备来源的同校配置请求，接口层只允许按DCDW内部编号寻址。
 */

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "config_command/command_parser.hpp"

namespace mqtt { class MqttClient; }

namespace config_command {

std::expected<nlohmann::json, CommandError> BuildConfigRequestPayload(
    std::string_view request_id, std::string_view target_dcdw_label,
    const ConfigParameterPatch& parameters);

bool PublishConfigRequest(mqtt::MqttClient& client,
                          const std::string& topic_namespace,
                          const std::string& source_vendor_id,
                          const std::string& request_id,
                          const std::string& target_dcdw_label,
                          const ConfigParameterPatch& parameters);

}  // namespace config_command
