#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fstream>
#include <iterator>
#include <string>

TEST_CASE("systemd服务启用正常退出重启") {
  std::ifstream input(SOURCE_DIR "/systemd/cns-rpi.service");
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  CHECK(text.find("Restart=always") != std::string::npos);
  CHECK(text.find("RestartSec=2") != std::string::npos);
  CHECK(text.find("--config-writer=helper") != std::string::npos);
  CHECK(text.find("--config-helper=/usr/local/libexec/cns-rpi-apply-config") !=
        std::string::npos);
}
