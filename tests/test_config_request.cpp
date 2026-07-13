#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "config_command/config_request.hpp"

TEST_CASE("设备请求只能表达内部编号目标") {
  config_command::ConfigParameterPatch parameters;
  parameters.heartbeat_interval_ms = 2000;
  auto payload = config_command::BuildConfigRequestPayload(
      "req-001", "DCDW-002", parameters);
  REQUIRE(payload.has_value());
  CHECK((*payload)["request_id"] == "req-001");
  CHECK((*payload)["target"] == nlohmann::json{{"dcdw_label", "DCDW-002"}});
  CHECK((*payload)["parameters"] ==
        nlohmann::json{{"heartbeat_interval_ms", 2000}});
  CHECK_FALSE(payload->contains("source_id"));
  CHECK_FALSE((*payload)["target"].contains("school_name"));
  CHECK_FALSE((*payload)["target"].contains("vendor_id"));
}

TEST_CASE("请求号目标编号和参数补丁必须合法") {
  config_command::ConfigParameterPatch parameters;
  parameters.mqtt_reconnect_delay_max_s = 60;
  CHECK_FALSE(config_command::BuildConfigRequestPayload("", "DCDW-002", parameters)
                  .has_value());
  CHECK_FALSE(config_command::BuildConfigRequestPayload(std::string(129, 'x'),
                                                         "DCDW-002", parameters)
                  .has_value());
  CHECK_FALSE(config_command::BuildConfigRequestPayload("req-1", "DCDW-2", parameters)
                  .has_value());
  CHECK_FALSE(config_command::BuildConfigRequestPayload(
                  "req-1", "DCDW-002", config_command::ConfigParameterPatch{})
                  .has_value());

  config_command::ConfigParameterPatch out_of_range;
  out_of_range.heartbeat_interval_ms = 99;
  CHECK_FALSE(config_command::BuildConfigRequestPayload(
                  "req-1", "DCDW-002", out_of_range).has_value());

  config_command::ConfigParameterPatch invalid_reconnect;
  invalid_reconnect.mqtt_reconnect_delay_s = 31;
  invalid_reconnect.mqtt_reconnect_delay_max_s = 30;
  CHECK_FALSE(config_command::BuildConfigRequestPayload(
                  "req-1", "DCDW-002", invalid_reconnect).has_value());
}
