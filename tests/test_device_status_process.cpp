#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <array>
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include "common/mavlink.h"

#include "logging/logger.hpp"
#include "mqtt/mqtt_client.hpp"
#include "device_status/health_tracker.hpp"

using namespace std::chrono_literals;

namespace {

class PiProcess {
 public:
  PiProcess(const std::filesystem::path& config, const std::filesystem::path& log) {
    pid_ = ::fork();
    if (pid_ == 0) {
      const int fd = ::open(log.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
      if (fd < 0) ::_exit(126);
      ::dup2(fd, STDOUT_FILENO);
      ::dup2(fd, STDERR_FILENO);
      ::close(fd);
      ::execl(CNS_RPI_EXECUTABLE, CNS_RPI_EXECUTABLE, config.c_str(), nullptr);
      ::_exit(127);
    }
    REQUIRE(pid_ > 0);
  }

  ~PiProcess() { if (pid_ > 0) Stop(SIGKILL); }

  int Stop(int signal) {
    ::kill(pid_, signal);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
    return status;
  }

 private:
  pid_t pid_ = -1;
};

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() / "cns-status-XXXXXX").string();
    REQUIRE(::mkdtemp(pattern.data()) != nullptr);
    path = pattern;
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
  std::filesystem::path path;
};

class F407Port {
 public:
  F407Port() {
    master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
    REQUIRE(master_ >= 0);
    REQUIRE(::grantpt(master_) == 0);
    REQUIRE(::unlockpt(master_) == 0);
    const auto* name = ::ptsname(master_);
    REQUIRE(name != nullptr);
    path = name;
  }
  ~F407Port() { ::close(master_); }

  void SendIdentity(const std::string& device_id) {
    mavlink_message_t heartbeat{};
    mavlink_msg_heartbeat_pack(1, 193, &heartbeat, MAV_TYPE_ONBOARD_CONTROLLER,
                              MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
    Write(heartbeat);
    std::array<std::uint8_t, 20> id_or_mac{};
    std::array<std::uint8_t, 20> uas_id{};
    std::copy(device_id.begin(), device_id.end(), uas_id.begin());
    mavlink_message_t identity{};
    mavlink_msg_open_drone_id_basic_id_pack(1, 193, &identity, 0, 0,
        id_or_mac.data(), MAV_ODID_ID_TYPE_SERIAL_NUMBER,
        MAV_ODID_UA_TYPE_HELICOPTER_OR_MULTIROTOR, uas_id.data());
    Write(identity);
  }

  void SendModuleFailure() {
    mavlink_message_t message{};
    const std::array<char, 10> name{'M', 'O', 'D', 'S', 'T', 'A', 'T', '0'};
    mavlink_msg_named_value_int_pack(1, 193, &message, 0, name.data(), 4);
    Write(message);
  }

  std::string path;

 private:
  void Write(const mavlink_message_t& message) {
    std::array<std::uint8_t, MAVLINK_MAX_PACKET_LEN> bytes{};
    const auto size = mavlink_msg_to_send_buffer(bytes.data(), &message);
    REQUIRE(::write(master_, bytes.data(), size) == size);
  }
  int master_ = -1;
};

std::optional<nlohmann::json> WaitFor(mqtt::MqttClient& client,
                                    const std::string& topic,
                                    const std::string& field,
                                    const std::string& value,
                                    std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto message = client.TryPopMessage(); message && message->topic == topic) {
      auto payload = nlohmann::json::parse(message->payload);
      if (payload.value(field, "") == value) return payload;
    }
    std::this_thread::sleep_for(10ms);
  }
  return std::nullopt;
}

}  // namespace

TEST_CASE("真实进程验证常规状态、离线遗嘱和首次绑定恢复") {
  const auto* port_text = std::getenv("CNS_TEST_MQTT_BROKER_PORT");
  if (!port_text) {
    MESSAGE("未设置CNS_TEST_MQTT_BROKER_PORT，跳过真实Broker进程检查");
    return;
  }
  TemporaryDirectory directory;
  const int port = std::stoi(port_text);
  const std::string device_id = "DCDWCNS1S2MEAG1VTA0C";
  const std::string topic_namespace = "cns_status_test_" + std::to_string(::getpid());
  const std::string prefix = topic_namespace + "/" + device_id;
  const std::string status_topic = prefix + "/runtime/status/v1";
  const auto config_path = directory.path / "config.json";
  const auto binding_path = directory.path / "binding.json";
  nlohmann::json config;
  std::ifstream(SOURCE_DIR "/config/config.example.json") >> config;
  config["serial"]["device"] = "/dev/cns-status-test-no-device";
  config["mqtt"]["connection"]["host"] = "127.0.0.1";
  config["mqtt"]["connection"]["port"] = port;
  config["mqtt"]["connection"]["keepalive"] = 5;
  config["mqtt"]["topics"]["namespace"] = topic_namespace;
  config["identity"]["binding_file"] = binding_path.string();
  config["identity"]["school_name"] = "示例职业技术学院";
  config["telemetry_publish"]["snapshot"]["enabled"] = false;
  config["telemetry_publish"]["realtime"]["enabled"] = false;
  std::ofstream(config_path) << config.dump();
  std::ofstream(binding_path) << nlohmann::json{
      {"schema_version", 1}, {"device_id", device_id}, {"device_type", "cns_box"}};

  std::ostringstream output;
  std::ostringstream errors;
  auto logger = logging::Logger::Create(
      {.minimum_level = logging::Level::kWarn, .file = {}, .max_file_size_bytes = 0}, output, errors);
  REQUIRE(logger.has_value());
  const auto options = [&](const std::string& name) {
    return mqtt::ConnectionOptions{
        .broker_host = "127.0.0.1", .broker_port = port,
        .client_id = topic_namespace + name, .username = "", .password = "",
        .keepalive_seconds = 5,
        .will = {.topic = topic_namespace + "/observer-will", .payload = "{}", .qos = 0, .retain = false},
        .subscriptions = {{prefix + "/#", 1}}};
  };
  auto observer = mqtt::MqttClient::Open(options("observer"), **logger);
  REQUIRE(observer.has_value());
  const auto connected_deadline = std::chrono::steady_clock::now() + 5s;
  while (!observer->IsConnected() && std::chrono::steady_clock::now() < connected_deadline) {
    std::this_thread::sleep_for(10ms);
  }
  REQUIRE(observer->IsConnected());
  REQUIRE(observer->PublishAndWait(prefix + "/probe", R"({"ready":"yes"})", 1, false, 2s));
  REQUIRE(WaitFor(*observer, prefix + "/probe", "ready", "yes", 2s));

  {
    PiProcess pi(config_path, directory.path / "graceful.log");
    auto first = WaitFor(*observer, status_topic, "online", "0", 5s);
    REQUIRE(first.has_value());
    CHECK(first->size() == 10);
    CHECK(first->at("school_id") == "示例职业技术学院");
    CHECK(first->at("device_status") == "0");
    CHECK(first->at("device_id") == device_id);
    CHECK(first->at("device_type") == "xn_box");
    CHECK(first->at("device_name") == "CNS 一代实训平台");
    CHECK(first->at("model_version") == EXPECTED_RPI_VERSION);
    CHECK_FALSE(first->contains("provisioned_at"));
    CHECK(first->at("product").is_null());
    CHECK(first->at("dcdw_label").is_null());
    const auto first_received = std::chrono::steady_clock::now();
    auto periodic = WaitFor(*observer, status_topic, "online", "0", 14s);
    REQUIRE(periodic.has_value());
    CHECK(*periodic == *first);
    CHECK(std::chrono::steady_clock::now() - first_received >= 9s);

    auto late_observer = mqtt::MqttClient::Open(options("late"), **logger);
    REQUIRE(late_observer.has_value());
    REQUIRE(WaitFor(*late_observer, status_topic, "online", "0", 2s));

    const int status = pi.Stop(SIGTERM);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    auto offline = WaitFor(*observer, status_topic, "online", "1", 3s);
    REQUIRE(offline.has_value());
    (*first)["online"] = "1";
    CHECK(*offline == *first);
  }
  {
    PiProcess pi(config_path, directory.path / "abrupt.log");
    REQUIRE(WaitFor(*observer, status_topic, "online", "0", 5s));
    const int status = pi.Stop(SIGKILL);
    CHECK(WIFSIGNALED(status));
    auto will = WaitFor(*observer, status_topic, "online", "1", 5s);
    REQUIRE(will.has_value());
    CHECK(will->size() == 10);
    CHECK(will->at("school_id") == "示例职业技术学院");
    CHECK(will->at("device_status") == "0");
    CHECK(will->at("device_id") == device_id);
    CHECK(will->at("device_type") == "xn_box");
    CHECK(will->at("device_name") == "CNS 一代实训平台");
    CHECK(will->at("model_version") == EXPECTED_RPI_VERSION);
    CHECK_FALSE(will->contains("provisioned_at"));
    CHECK(will->at("product").is_null());
    CHECK_FALSE(will->contains("control_status"));
  }
  {
    const auto health_path = binding_path.string() + ".health.json";
    REQUIRE(std::filesystem::exists(health_path));
    device_status::HealthTracker saved_health(health_path);
    std::string error;
    REQUIRE(saved_health.Load(device_id, &error));
    std::array<std::uint8_t, state::kModuleCount> modules{};
    modules[0] = 5;
    const auto start = device_status::HealthClock::time_point{};
    saved_health.Update(modules, start);
    saved_health.Update(modules, start + 12h);
    REQUIRE(saved_health.Status() == "2");
    REQUIRE(saved_health.Save(&error));
    config["device_status"] = {
        {"device_name", "教学一号平台"}, {"device_type", "教学箱"},
        {"dcdw_label", "一号实验台"}, {"capabilities", {"telemetry"}},
        {"product", {{"manufacturer_code", "厂家"}, {"model_code", "型号"}}}};
    std::ofstream(config_path) << config.dump();
    PiProcess pi(config_path, directory.path / "restored-damaged.log");
    auto online = WaitFor(*observer, status_topic, "device_status", "2", 5s);
    REQUIRE(online.has_value());
    CHECK(online->at("online") == "0");
    CHECK(online->at("device_name") == "教学一号平台");
    CHECK(online->at("device_type") == "教学箱");
    CHECK(online->at("dcdw_label") == "一号实验台");
    CHECK(online->at("capabilities") == nlohmann::json::array({"telemetry"}));
    CHECK(online->at("product").at("model_code") == "型号");
    CHECK(online->at("model_version") == EXPECTED_RPI_VERSION);
    pi.Stop(SIGKILL);
    const auto will = WaitFor(*observer, status_topic, "online", "1", 5s);
    REQUIRE(will.has_value());
    (*online)["online"] = "1";
    CHECK(*will == *online);

    REQUIRE(saved_health.SetMaintenance(true, &error));
    PiProcess maintained(config_path, directory.path / "restored-maintenance.log");
    auto maintenance = WaitFor(*observer, status_topic, "online", "0", 5s);
    REQUIRE(maintenance.has_value());
    CHECK(maintenance->at("device_status") == "1");
    CHECK(maintenance->at("device_name") == "教学一号平台");
    const int status = maintained.Stop(SIGTERM);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    const auto offline = WaitFor(*observer, status_topic, "online", "1", 3s);
    REQUIRE(offline.has_value());
    CHECK(offline->at("device_status") == "1");
    device_status::HealthTracker restored(health_path);
    REQUIRE(restored.Load(device_id, &error));
    CHECK(restored.Status() == "1");
  }
  {
    device_status::HealthTracker progress(binding_path.string() + ".health.json");
    REQUIRE(progress.Load(device_id, nullptr));
    REQUIRE(progress.SetMaintenance(false, nullptr));
    std::array<std::uint8_t, state::kModuleCount> modules{};
    modules.fill(2);
    const auto start = device_status::HealthClock::time_point{};
    progress.Update(modules, start);
    modules[0] = 4;
    progress.Update(modules, start);
    progress.Update(modules, start + 12h - 1500ms);
    REQUIRE(progress.Status() == "0");
    REQUIRE(progress.Save(nullptr));
    F407Port f407;
    config.erase("device_status");
    config["serial"]["device"] = f407.path;
    std::ofstream(config_path) << config.dump();
    while (observer->TryPopMessage()) {}
    PiProcess pi(config_path, directory.path / "health-transition.log");
    auto initial = WaitFor(*observer, status_topic, "online", "0", 5s);
    REQUIRE(initial.has_value());
    CHECK(initial->at("device_status") == "0");
    std::optional<nlohmann::json> damaged;
    for (int attempt = 0; attempt < 8 && !damaged; ++attempt) {
      f407.SendIdentity(device_id);
      f407.SendModuleFailure();
      damaged = WaitFor(*observer, status_topic, "device_status", "2", 1s);
    }
    REQUIRE(damaged.has_value());
    CHECK(damaged->at("online") == "0");
    pi.Stop(SIGKILL);
    const auto will = WaitFor(*observer, status_topic, "online", "1", 5s);
    REQUIRE(will.has_value());
    CHECK(will->at("device_status") == "2");
  }
  {
    F407Port f407;
    config.erase("device_status");
    const auto delayed_binding = directory.path / "later" / "binding.json";
    config["serial"]["device"] = f407.path;
    config["identity"]["binding_file"] = delayed_binding.string();
    std::ofstream(config_path) << config.dump();
    while (observer->TryPopMessage()) {}
    PiProcess pi(config_path, directory.path / "delayed-binding.log");
    std::optional<nlohmann::json> online;
    for (int attempt = 0; attempt < 5 && !online; ++attempt) {
      f407.SendIdentity(device_id);
      online = WaitFor(*observer, status_topic, "online", "0", 1s);
    }
    REQUIRE(online.has_value());
    CHECK(online->at("dcdw_label") == "DCDW-001");
    CHECK(online->at("product").at("model_code") == "CNS1");
    REQUIRE_FALSE(std::filesystem::exists(delayed_binding));
    std::filesystem::create_directory(delayed_binding.parent_path());
    const auto bound_deadline = std::chrono::steady_clock::now() + 5s;
    while (!std::filesystem::exists(delayed_binding) &&
           std::chrono::steady_clock::now() < bound_deadline) {
      f407.SendIdentity(device_id);
      std::this_thread::sleep_for(100ms);
    }
    REQUIRE(std::filesystem::exists(delayed_binding));
    const nlohmann::json request{
        {"schema_version", 1},
        {"command_id", "7d15e276-c7e1-4f52-aaee-7314a6417667"},
        {"request_id", "f886f45b-01f8-4a54-b2b6-a65b07477aa1"},
        {"target", {{"device_id", device_id}}},
        {"session_id", "c4191037-9e2a-480c-a995-2174e4672563"},
        {"action_id", "f886f45b-01f8-4a54-b2b6-a65b07477aa1"},
        {"lease_version", 3}, {"operation", "inspect_pi_link"},
        {"expires_at", "2099-01-01T00:00:00.000Z"},
    };
    std::optional<nlohmann::json> ack;
    for (int attempt = 0; attempt < 3 && !ack; ++attempt) {
      REQUIRE(observer->PublishAndWait(prefix + "/experiment/set",
                                       request.dump(), 1, false, 2s));
      ack = WaitFor(*observer, prefix + "/experiment/ack", "command_id",
                    request.at("command_id").get<std::string>(), 2s);
    }
    REQUIRE(ack.has_value());
    CHECK(ack->at("status") == "completed");
    const int status = pi.Stop(SIGTERM);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
  }
}
