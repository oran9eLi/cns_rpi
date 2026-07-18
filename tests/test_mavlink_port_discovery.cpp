#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "uart/mavlink_port_discovery.hpp"

#include <string>
#include <vector>

TEST_CASE("USB候选按数值自然排序后从两端交替") {
  const std::vector<std::string> input{
      "/dev/ttyUSB10", "/dev/ttyUSB2", "/dev/ttyUSB1",
      "/dev/ttyUSB3",  "/dev/ttyACM2", "/dev/ttyACM0"};
  CHECK(uart::OrderSerialCandidates(input) ==
        std::vector<std::string>{
            "/dev/ttyUSB1", "/dev/ttyUSB10", "/dev/ttyUSB2",
            "/dev/ttyUSB3", "/dev/ttyACM0",  "/dev/ttyACM2"});
}

TEST_CASE("候选排序去重且奇数数量不遗漏中间项") {
  const std::vector<std::string> input{
      "/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB1",
      "/dev/ttyUSB2", "/dev/ttyUSB3", "/dev/ttyUSB4"};
  CHECK(uart::OrderSerialCandidates(input) ==
        std::vector<std::string>{
            "/dev/ttyUSB0", "/dev/ttyUSB4", "/dev/ttyUSB1",
            "/dev/ttyUSB3", "/dev/ttyUSB2"});
}
