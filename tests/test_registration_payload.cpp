#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "registration/registration_payload.hpp"

TEST_CASE("online注册包含设备元数据") {
  registration::OnlineRegistration input{
      .device_id = "DCDWCNS1ABCDEFGHIJKL",
      .device_type = device::Type::kCnsBox,
      .vendor_id = "ABC123",
      .school_name = "NNUTC",
      .dcdw_label = "DCDW-001",
      .remote_id = std::nullopt,
      .gateway_id = "100000001234abcd",
      .system_id = 1,
      .component_id = 193,
      .mavlink_version = 3,
      .autopilot_version = std::nullopt,
  };
  const auto payload =
      nlohmann::json::parse(registration::BuildOnlinePayload(input));
  CHECK(payload["schema_version"] == 2);
  CHECK(payload["device_id"] == "DCDWCNS1ABCDEFGHIJKL");
  CHECK(payload["device_type"] == "cns_box");
  CHECK(payload["status"] == "online");
  CHECK(payload["identity"]["vendor_id"] == "ABC123");
  CHECK(payload["identity"]["school_name"] == "NNUTC");
  CHECK(payload["identity"]["dcdw_label"] == "DCDW-001");
  CHECK(payload["endpoint"]["sysid"] == 1);
  CHECK(payload["endpoint"]["compid"] == 193);
  CHECK(payload["gateway"]["gateway_id"] == "100000001234abcd");
  CHECK(payload["capabilities"] ==
        nlohmann::json{"telemetry", "remote_id", "runtime_config",
                       "box_private_control"});
}

TEST_CASE("角色号未就绪时online注册省略该字段") {
  registration::OnlineRegistration input{
      .device_id = "DCDWCNS1ABCDEFGHIJKL",
      .device_type = device::Type::kCnsBox,
      .vendor_id = "ABC123",
      .school_name = "NNUTC",
      .dcdw_label = std::nullopt,
      .remote_id = std::nullopt,
      .gateway_id = std::nullopt,
      .system_id = std::nullopt,
      .component_id = std::nullopt,
      .mavlink_version = std::nullopt,
      .autopilot_version = std::nullopt,
  };
  const auto payload = nlohmann::json::parse(registration::BuildOnlinePayload(input));
  CHECK_FALSE(payload["identity"].contains("dcdw_label"));
  CHECK(payload["status"] == "online");
}

TEST_CASE("offline注册只携带主键和状态") {
  CHECK(nlohmann::json::parse(registration::BuildOfflinePayload(
            "PX4U1-0123456789ABCDEF",
            device::Type::kFlightController)) == nlohmann::json{
            {"schema_version", 2},
            {"device_id", "PX4U1-0123456789ABCDEF"},
            {"device_type", "flight_controller"},
            {"status", "offline"},
        });
}

TEST_CASE("PX4在线注册携带硬件身份且不要求学校") {
  mavlink_autopilot_version_t version{};
  version.uid = 0x0123456789ABCDEFULL;
  version.uid2[0] = 0xAA;
  version.flight_sw_version = (1U << 24) | (17U << 16) | (3U << 8);
  version.vendor_id = 26;
  version.product_id = 7;

  registration::OnlineRegistration input{
      .device_id =
          "PX4U2-AA0000000000000000000000000000000000",
      .device_type = device::Type::kFlightController,
      .vendor_id = std::nullopt,
      .school_name = std::nullopt,
      .dcdw_label = std::nullopt,
      .remote_id = "RID-001",
      .gateway_id = std::nullopt,
      .system_id = 1,
      .component_id = 1,
      .mavlink_version = 3,
      .autopilot_version = version,
  };
  const auto payload =
      nlohmann::json::parse(registration::BuildOnlinePayload(input));
  CHECK(payload["device_type"] == "flight_controller");
  CHECK(payload["identity"]["uid2"] ==
        "AA0000000000000000000000000000000000");
  CHECK(payload["identity"]["uid"] == "0123456789ABCDEF");
  CHECK(payload["identity"]["firmware_version"] == "1.17.3");
  CHECK_FALSE(payload["identity"].contains("school_name"));
  CHECK(payload["capabilities"] ==
        nlohmann::json{"telemetry", "remote_id", "px4_official_control"});
}

TEST_CASE("Client ID由配置前缀和vendor_id组成") {
  CHECK(registration::BuildClientId("cns-rpi", "ABC123") == "cns-rpi-ABC123");
}

TEST_CASE("设备标识必须能安全用于topic和Client ID") {
  CHECK(registration::IsValidDeviceIdentity("cns-rpi", "ABC123"));
  CHECK_FALSE(registration::IsValidDeviceIdentity("cns-rpi", ""));
  CHECK_FALSE(registration::IsValidDeviceIdentity("cns-rpi", "bad/id"));
  CHECK_FALSE(registration::IsValidDeviceIdentity("cns-rpi", "bad+id"));
  CHECK_FALSE(registration::IsValidDeviceIdentity("cns-rpi", "bad#id"));
  CHECK_FALSE(registration::IsValidDeviceIdentity("cns-rpi", std::string("bad\0id", 6)));
  CHECK_FALSE(registration::IsValidDeviceIdentity("", "ABC123"));
}
