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

TEST_CASE("ReadRpiSerial从fixture文件正确解析Serial行") {
  auto serial = protocol::ReadRpiSerial("tests/fixtures/cpuinfo_with_serial.txt");
  REQUIRE(serial.has_value());
  CHECK(*serial == "100000001234abcd");
}

TEST_CASE("ReadRpiSerial在没有Serial行的文件里返回nullopt") {
  auto serial = protocol::ReadRpiSerial("tests/fixtures/cpuinfo_without_serial.txt");
  CHECK_FALSE(serial.has_value());
}

TEST_CASE("ReadRpiSerial在文件不存在时返回nullopt") {
  auto serial = protocol::ReadRpiSerial("tests/fixtures/cpuinfo_does_not_exist.txt");
  CHECK_FALSE(serial.has_value());
}

TEST_CASE("ExtractVendorId从uas_id中间有null的情况正确提取") {
  std::uint8_t uas_id[20] = {'D', 'C', 'D', 'W', 'C', 'N', 'S', '1', 'A', 'B',
                              'C', 'D', 'E', 'F', 0, 0, 0, 0, 0, 0};
  CHECK(protocol::ExtractVendorId(uas_id) == "DCDWCNS1ABCDEF");
}

TEST_CASE("ExtractVendorId在uas_id写满20字节无null终止符时提取整20字节") {
  std::uint8_t uas_id[20] = {'D', 'C', 'D', 'W', 'C', 'N', 'S', '1', 'A', 'B',
                              'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K', 'L', 'M'};
  auto result = protocol::ExtractVendorId(uas_id);
  CHECK(result.size() == 20);
  CHECK(result == "DCDWCNS1ABCDEFGHJKLM");
}

TEST_CASE("主控箱vendor id必须是20个ASCII字母数字") {
  CHECK(protocol::IsValidCnsBoxVendorId("DCDWCNS1ABCDEFGHJKLM"));
  CHECK_FALSE(protocol::IsValidCnsBoxVendorId("DCDWCNS1"));
  CHECK_FALSE(protocol::IsValidCnsBoxVendorId("DCDWCNS1ABCDEFGHJK/M"));
}

TEST_CASE("PX4设备ID优先uid2并在uid2无效时回退uid") {
  mavlink_autopilot_version_t version{};
  version.uid = 0x0123456789ABCDEFULL;
  version.uid2[0] = 0x01;
  version.uid2[17] = 0xAB;

  CHECK(protocol::FormatPx4Uid2(version) ==
        "0100000000000000000000000000000000AB");
  CHECK(protocol::FormatPx4Uid(version) == "0123456789ABCDEF");
  CHECK(protocol::FormatPx4DeviceId(version) ==
        "PX4U2-0100000000000000000000000000000000AB");

  version.uid2[0] = 0;
  version.uid2[17] = 0;
  CHECK(protocol::FormatPx4DeviceId(version) ==
        "PX4U1-0123456789ABCDEF");
  version.uid = 0;
  CHECK_FALSE(protocol::FormatPx4DeviceId(version).has_value());
}

TEST_CASE("PX4身份请求使用标准MAV_CMD_REQUEST_MESSAGE") {
  const control_command::MavlinkEndpoint target{2, MAV_COMP_ID_AUTOPILOT1};
  const auto message = protocol::BuildAutopilotVersionRequest(
      2, MAV_COMP_ID_ONBOARD_COMPUTER, target);
  CHECK(message.msgid == MAVLINK_MSG_ID_COMMAND_LONG);

  mavlink_command_long_t command{};
  mavlink_msg_command_long_decode(&message, &command);
  CHECK(command.target_system == 2);
  CHECK(command.target_component == MAV_COMP_ID_AUTOPILOT1);
  CHECK(command.command == MAV_CMD_REQUEST_MESSAGE);
  CHECK(command.param1 ==
        doctest::Approx(MAVLINK_MSG_ID_AUTOPILOT_VERSION));
}
