#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "protocol/identity.hpp"
#include "protocol/px4_identity.hpp"

TEST_CASE("FormatDcdwLabel对sysid=0补零到DCDW-000") {
  CHECK(protocol::FormatDcdwLabel(0) == "DCDW-000");
}

TEST_CASE("FormatDcdwLabel对sysid=1补零到DCDW-001") {
  CHECK(protocol::FormatDcdwLabel(1) == "DCDW-001");
}

TEST_CASE("FormatDcdwLabel对sysid=255(uint8_t最大值)输出DCDW-255") {
  CHECK(protocol::FormatDcdwLabel(255) == "DCDW-255");
}

TEST_CASE("ExtractUasId从uas_id中间有null的情况正确提取") {
  std::uint8_t uas_id[20] = {'D', 'C', 'D', 'W', 'C', 'N', 'S', '1', 'A', 'B',
                              'C', 'D', 'E', 'F', 0, 0, 0, 0, 0, 0};
  CHECK(protocol::ExtractUasId(uas_id) == "DCDWCNS1ABCDEF");
}

TEST_CASE("ExtractUasId在uas_id写满20字节无null终止符时提取整20字节") {
  std::uint8_t uas_id[20] = {'D', 'C', 'D', 'W', 'C', 'N', 'S', '1', 'A', 'B',
                              'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K', 'L', 'M'};
  auto result = protocol::ExtractUasId(uas_id);
  CHECK(result.size() == 20);
  CHECK(result == "DCDWCNS1ABCDEFGHJKLM");
}

TEST_CASE("IsValidUasId对两类设备使用同一套校验,不要求固定长度") {
  // 主控箱的 20 位厂商码和 PX4 的任意短 Remote ID 都必须通过。
  CHECK(protocol::IsValidUasId("DCDWCNS1ABCDEFGHJKLM"));
  CHECK(protocol::IsValidUasId("PX4RID001"));
  CHECK(protocol::IsValidUasId("a"));
}

TEST_CASE("IsValidUasId拒绝空值,超长值和不能用于topic的字符") {
  CHECK_FALSE(protocol::IsValidUasId(""));
  CHECK_FALSE(protocol::IsValidUasId(std::string(21, 'A')));
  CHECK_FALSE(protocol::IsValidUasId("bad/id"));   // MQTT 层级分隔符
  CHECK_FALSE(protocol::IsValidUasId("bad+id"));   // 单层通配符
  CHECK_FALSE(protocol::IsValidUasId("bad#id"));   // 多层通配符
  CHECK_FALSE(protocol::IsValidUasId("bad id"));
  CHECK_FALSE(protocol::IsValidUasId(std::string("bad\0id", 6)));
}

TEST_CASE("IsValidUasId允许连字符下划线点和冒号") {
  CHECK(protocol::IsValidUasId("a-b_c.d:e"));
}

TEST_CASE("主控箱产品前缀只用于告警,不参与合法性判断") {
  CHECK(protocol::HasCnsBoxProductPrefix("DCDWCNS1ABCDEFGHJKLM"));
  CHECK_FALSE(protocol::HasCnsBoxProductPrefix("PX4RID001"));
  // 前缀不匹配不影响 uas_id 本身是否可用作 device_id。
  CHECK(protocol::IsValidUasId("PX4RID001"));
}

TEST_CASE("CnsBoxProductFrom从device_id前缀拆出产品信息") {
  const auto product = protocol::CnsBoxProductFrom("DCDWCNS1ABCDEFGHJKLM");
  REQUIRE(product.has_value());
  CHECK(product->manufacturer_code == "DCDW");
  CHECK(product->model_code == "CNS1");

  CHECK_FALSE(protocol::CnsBoxProductFrom("PX4RID001").has_value());
}

TEST_CASE("PX4产品信息转十进制字符串,全0时不输出") {
  mavlink_autopilot_version_t version{};
  version.vendor_id = 26;
  version.product_id = 7;

  const auto product = protocol::ExtractPx4ProductInfo(version);
  REQUIRE(product.has_value());
  CHECK(product->manufacturer_code == "26");
  CHECK(product->model_code == "7");

  mavlink_autopilot_version_t unknown{};
  CHECK_FALSE(protocol::ExtractPx4ProductInfo(unknown).has_value());
}

TEST_CASE("PX4版本信息按语义版本格式化,单字段缺失时各自省略") {
  mavlink_autopilot_version_t version{};
  version.flight_sw_version = (1U << 24) | (17U << 16) | (3U << 8);
  version.board_version = 42;

  auto info = protocol::ExtractPx4VersionInfo(version);
  REQUIRE(info.has_value());
  CHECK(info->firmware == "1.17.3");
  CHECK(info->hardware == "42");

  version.board_version = 0;
  info = protocol::ExtractPx4VersionInfo(version);
  REQUIRE(info.has_value());
  CHECK_FALSE(info->hardware.has_value());
  CHECK(info->firmware == "1.17.3");

  mavlink_autopilot_version_t unknown{};
  CHECK_FALSE(protocol::ExtractPx4VersionInfo(unknown).has_value());
}

TEST_CASE("身份与版本请求共用MAV_CMD_REQUEST_MESSAGE,只有param1不同") {
  const auto basic_id_request = protocol::BuildMessageRequest(
      MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID, 2, MAV_COMP_ID_ONBOARD_COMPUTER, 2,
      MAV_COMP_ID_AUTOPILOT1);
  CHECK(basic_id_request.msgid == MAVLINK_MSG_ID_COMMAND_LONG);

  mavlink_command_long_t command{};
  mavlink_msg_command_long_decode(&basic_id_request, &command);
  CHECK(command.target_system == 2);
  CHECK(command.target_component == MAV_COMP_ID_AUTOPILOT1);
  CHECK(command.command == MAV_CMD_REQUEST_MESSAGE);
  CHECK(command.param1 ==
        doctest::Approx(MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID));

  const auto version_request = protocol::BuildMessageRequest(
      MAVLINK_MSG_ID_AUTOPILOT_VERSION, 2, MAV_COMP_ID_ONBOARD_COMPUTER, 2,
      MAV_COMP_ID_AUTOPILOT1);
  mavlink_msg_command_long_decode(&version_request, &command);
  CHECK(command.command == MAV_CMD_REQUEST_MESSAGE);
  CHECK(command.param1 == doctest::Approx(MAVLINK_MSG_ID_AUTOPILOT_VERSION));
}
