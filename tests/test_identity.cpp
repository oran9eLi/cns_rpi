#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "protocol/identity.hpp"

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
