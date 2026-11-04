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

TEST_CASE("IsValidUasId不要求固定长度或主控箱产品前缀") {
  CHECK(protocol::IsValidUasId("DCDWCNS1ABCDEFGHJKLM"));
  CHECK(protocol::IsValidUasId("TESTUAS001"));
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
  CHECK_FALSE(protocol::HasCnsBoxProductPrefix("TESTUAS001"));
  // 前缀不匹配不影响 uas_id 本身是否可用作 device_id。
  CHECK(protocol::IsValidUasId("TESTUAS001"));
}

TEST_CASE("CnsBoxProductFrom从device_id前缀拆出产品信息") {
  const auto product = protocol::CnsBoxProductFrom("DCDWCNS1ABCDEFGHJKLM");
  REQUIRE(product.has_value());
  CHECK(product->manufacturer_code == "DCDW");
  CHECK(product->model_code == "CNS1");

  CHECK_FALSE(protocol::CnsBoxProductFrom("TESTUAS001").has_value());
}
