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
  CHECK(text.find("cellular-dialup.service") == std::string::npos);
}

TEST_CASE("部署脚本安装生产helper和systemd服务") {
  std::ifstream input(SOURCE_DIR "/scripts/deploy.sh");
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  CHECK(text.find("sudo -v") != std::string::npos);
  CHECK(text.find("sudo -n true") != std::string::npos);
  CHECK(text.find("请使用 dcdw 普通用户执行本脚本") != std::string::npos);
  CHECK(text.find("EXPECTED_REPO_ROOT=\"/home/dcdw/cns_rpi\"") != std::string::npos);
  CHECK(text.find("cns-rpi-apply-config") != std::string::npos);
  CHECK(text.find("/etc/systemd/system/cns-rpi.service") != std::string::npos);
  CHECK(text.find("/etc/systemd/system/cellular-dialup.service") !=
        std::string::npos);
  CHECK(text.find("systemctl daemon-reload") != std::string::npos);
  CHECK(text.find("systemctl enable cns-rpi.service") != std::string::npos);
  CHECK(text.find("systemctl enable cellular-dialup.service") !=
        std::string::npos);
  CHECK(text.find("systemctl is-active --quiet cellular-dialup.service") !=
        std::string::npos);
  CHECK(text.find("systemctl restart cellular-dialup.service") != std::string::npos);
  CHECK(text.find("systemctl start cellular-dialup.service") != std::string::npos);
}

TEST_CASE("5G拨号单元是读取持久配置的常驻服务") {
  std::ifstream input(SOURCE_DIR "/systemd/cellular-dialup.service");
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  CHECK(text.find("Type=simple") != std::string::npos);
  CHECK(text.find("User=dcdw") != std::string::npos);
  CHECK(text.find("RuntimeDirectory=cns-rpi") != std::string::npos);
  CHECK(text.find("Environment=PYTHONUNBUFFERED=1") != std::string::npos);
  CHECK(text.find("/var/lib/cns-rpi/config.json") != std::string::npos);
  CHECK(text.find("Restart=on-failure") != std::string::npos);
  CHECK(text.find("RemainAfterExit=yes") == std::string::npos);
}

TEST_CASE("共享运行目录不随单个服务停止而删除") {
  for (const auto* unit : {"/systemd/cns-rpi.service",
                           "/systemd/cellular-dialup.service"}) {
    std::ifstream input(std::string(SOURCE_DIR) + unit);
    const std::string text{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
    CAPTURE(unit);
    CHECK(text.find("RuntimeDirectory=cns-rpi") != std::string::npos);
    CHECK(text.find("RuntimeDirectoryPreserve=yes") != std::string::npos);
  }
}

TEST_CASE("依赖安装脚本复用部署脚本") {
  std::ifstream input(SOURCE_DIR "/scripts/install_deps.sh");
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  CHECK(text.find("deploy.sh") != std::string::npos);
}
