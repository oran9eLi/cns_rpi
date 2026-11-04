#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "registration/registration_payload.hpp"

TEST_CASE("主控箱online注册使用v3扁平结构") {
  registration::OnlineRegistration input{
      .device_id = "DCDWCNS1S2MEAG1VTA0C",
      .device_type = device::Type::kCnsBox,
      .school_name = "NNUTC",
      .dcdw_label = "DCDW-001",
      .product = device::ProductInfo{.manufacturer_code = "DCDW",
                                     .model_code = "CNS1"},
  };
  const auto payload =
      nlohmann::json::parse(registration::BuildOnlinePayload(input));
  CHECK(payload["schema_version"] == 3);
  CHECK(payload["device_id"] == "DCDWCNS1S2MEAG1VTA0C");
  CHECK(payload["device_type"] == "cns_box");
  CHECK(payload["status"] == "online");
  CHECK(payload["school_name"] == "NNUTC");
  CHECK(payload["dcdw_label"] == "DCDW-001");
  CHECK(payload["product"]["manufacturer_code"] == "DCDW");
  CHECK(payload["product"]["model_code"] == "CNS1");
  CHECK(payload["capabilities"] ==
        nlohmann::json{"telemetry", "remote_id", "runtime_config",
                       "box_private_control"});
}

TEST_CASE("online注册不再输出已删除的身份字段和树莓派信息") {
  registration::OnlineRegistration input{
      .device_id = "DCDWCNS1S2MEAG1VTA0C",
      .device_type = device::Type::kCnsBox,
      .school_name = "NNUTC",
      .dcdw_label = "DCDW-001",
      .product = std::nullopt,
  };
  const auto text = registration::BuildOnlinePayload(input);
  const auto payload = nlohmann::json::parse(text);

  for (const char* removed : {"identity", "endpoint", "gateway"}) {
    CHECK_FALSE(payload.contains(removed));
  }
  // 这些键在任何嵌套层级都不应再出现；capabilities 里的 remote_id 是能力名
  // 而不是字段键，所以按 JSON 键的写法匹配。
  for (const char* removed : {"\"vendor_id\":", "\"remote_id\":", "\"uid\":",
                              "\"uid2\":", "\"gateway_id\":", "\"rpi_serial\":",
                              "\"sysid\":", "\"compid\":"}) {
    CHECK(text.find(removed) == std::string::npos);
  }
  CHECK(payload["capabilities"].front() == "telemetry");
}

TEST_CASE("角色号和学校未就绪时online注册省略该字段") {
  registration::OnlineRegistration input{
      .device_id = "DCDWCNS1S2MEAG1VTA0C",
      .device_type = device::Type::kCnsBox,
      .school_name = std::nullopt,
      .dcdw_label = std::nullopt,
      .product = std::nullopt,
  };
  const auto payload = nlohmann::json::parse(registration::BuildOnlinePayload(input));
  CHECK_FALSE(payload.contains("dcdw_label"));
  CHECK_FALSE(payload.contains("school_name"));
  CHECK_FALSE(payload.contains("product"));
  CHECK(payload["status"] == "online");
}

TEST_CASE("offline注册只携带主键和状态") {
  CHECK(nlohmann::json::parse(registration::BuildOfflinePayload(
            "DCDWCNS1S2MEAG1VTA0C",
            device::Type::kCnsBox)) == nlohmann::json{
            {"schema_version", 3},
            {"device_id", "DCDWCNS1S2MEAG1VTA0C"},
            {"device_type", "cns_box"},
            {"status", "offline"},
        });
}

TEST_CASE("Client ID由配置前缀和device_id组成") {
  CHECK(registration::BuildClientId("cns-rpi", "DCDWCNS1S2MEAG1VTA0C") ==
        "cns-rpi-DCDWCNS1S2MEAG1VTA0C");
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
