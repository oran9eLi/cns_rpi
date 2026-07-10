#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "registration/registration_payload.hpp"

TEST_CASE("online注册包含设备元数据") {
  registration::OnlineRegistration input{
      .vendor_id = "ABC123",
      .school_name = "NNUTC",
      .dcdw_label = "DCDW-001",
  };
  CHECK(nlohmann::json::parse(registration::BuildOnlinePayload(input)) == nlohmann::json{
            {"schema_version", 1},
            {"vendor_id", "ABC123"},
            {"school_name", "NNUTC"},
            {"dcdw_label", "DCDW-001"},
            {"status", "online"},
        });
}

TEST_CASE("角色号未就绪时online注册省略该字段") {
  registration::OnlineRegistration input{
      .vendor_id = "ABC123",
      .school_name = "NNUTC",
      .dcdw_label = std::nullopt,
  };
  const auto payload = nlohmann::json::parse(registration::BuildOnlinePayload(input));
  CHECK_FALSE(payload.contains("dcdw_label"));
  CHECK(payload["status"] == "online");
}

TEST_CASE("offline注册只携带主键和状态") {
  CHECK(nlohmann::json::parse(registration::BuildOfflinePayload("ABC123")) == nlohmann::json{
            {"schema_version", 1},
            {"vendor_id", "ABC123"},
            {"status", "offline"},
        });
}

TEST_CASE("Client ID由配置前缀和vendor_id组成") {
  CHECK(registration::BuildClientId("cns-rpi", "ABC123") == "cns-rpi-ABC123");
}
