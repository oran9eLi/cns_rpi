#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <nlohmann/json.hpp>

#include "device_status/device_status.hpp"
#include "registration/registration_payload.hpp"

using namespace std::chrono_literals;

namespace {

registration::OnlineRegistration Box() {
  return {.device_id = "DCDWCNS1S2MEAG1VTA0C",
          .device_type = device::Type::kCnsBox,
          .school_name = "示例职业技术学院",
          .dcdw_label = "DCDW-001",
          .product = device::ProductInfo{.manufacturer_code = "DCDW",
                                         .model_code = "CNS1"}};
}

constexpr auto kStart = device_status::Clock::time_point{};

}  // namespace

TEST_CASE("常规状态严格包含当前约定的十一个字段") {
  const auto payload = nlohmann::json::parse(device_status::BuildPayload(Box(), "2.3.4", true));
  CHECK(payload == nlohmann::json{
                       {"device_id", "DCDWCNS1S2MEAG1VTA0C"},
                       {"school_id", "示例职业技术学院"},
                       {"dcdw_label", "DCDW-001"},
                       {"model_version", "2.3.4"},
                       {"device_name", "CNS 一代实训平台"},
                       {"device_type", "cns_box"},
                       {"capabilities", {"telemetry", "remote_id", "runtime_config",
                                         "box_private_control"}},
                       {"product", {{"manufacturer_code", "DCDW"}, {"model_code", "CNS1"}}},
                       {"online", "0"},
                       {"device_status", "0"},
                       {"fault_modules", nullptr},
                   });
  const auto registration = nlohmann::json::parse(registration::BuildOnlinePayload(Box()));
  CHECK(payload.at("capabilities") == registration.at("capabilities"));
  CHECK(registration.at("device_type") == "cns_box");
}

TEST_CASE("缺失元数据保留null而不是猜测或借用学校名称") {
  auto box = Box();
  box.dcdw_label.reset();
  box.product.reset();
  const auto payload = nlohmann::json::parse(device_status::BuildPayload(box, "2.3.4", true));
  CHECK(payload.at("dcdw_label").is_null());
  CHECK(payload.at("product").is_null());
  CHECK(payload.at("model_version") == "2.3.4");
  CHECK(payload.at("device_name") == "CNS 一代实训平台");
  for (const auto* removed : {"version", "provisioned_at", "school_name"}) {
    CHECK_FALSE(payload.contains(removed));
  }
}

TEST_CASE("正常离线只改变online字符串保留设备资料") {
  auto online = nlohmann::json::parse(device_status::BuildPayload(Box(), "2.3.4", true));
  const auto offline = nlohmann::json::parse(device_status::BuildPayload(Box(), "2.3.4", false));
  online["online"] = "1";
  CHECK(offline == online);
}

TEST_CASE("异常模块字段保留未知空列表多模块及正常离线资料") {
  const auto unknown = nlohmann::json::parse(device_status::BuildPayload(Box(), "2.3.4", true));
  CHECK(unknown.at("fault_modules").is_null());
  const std::vector<std::string> none;
  const auto healthy = nlohmann::json::parse(
      device_status::BuildPayload(Box(), "2.3.4", true, {}, "0", none));
  CHECK(healthy.at("fault_modules") == nlohmann::json::array());
  const std::vector<std::string> modules{"GNSS", "IMU"};
  auto online = nlohmann::json::parse(
      device_status::BuildPayload(Box(), "2.3.4", true, {}, "0", modules));
  CHECK(online.at("fault_modules") == nlohmann::json::array({"GNSS", "IMU"}));
  CHECK(online.at("device_status") == "0");
  const auto offline = nlohmann::json::parse(
      device_status::BuildPayload(Box(), "2.3.4", false, {}, "0", modules));
  online["online"] = "1";
  CHECK(offline == online);
  const auto maintained = nlohmann::json::parse(
      device_status::BuildPayload(Box(), "2.3.4", true, {}, "1", modules));
  CHECK(maintained.at("fault_modules") == online.at("fault_modules"));
}

TEST_CASE("整机仍可用时异常模块变化也立即触发状态上报") {
  const std::vector<std::string> none;
  const std::vector<std::string> faults{"GNSS"};
  const auto normal = device_status::BuildPayload(Box(), "2.3.4", true, {}, "0", none);
  const auto faulty = device_status::BuildPayload(Box(), "2.3.4", true, {}, "0", faults);
  device_status::PublicationState state;
  state.MarkPublished(1, normal, kStart);
  CHECK(state.ShouldPublish(true, 1, faulty, kStart + 1s));
  state.MarkPublished(1, faulty, kStart + 1s);
  CHECK_FALSE(state.ShouldPublish(true, 1, faulty, kStart + 2s));
  CHECK(state.ShouldPublish(true, 1, normal, kStart + 2s));
}

TEST_CASE("资料覆盖只改变可编辑字段并保留健康和连接状态的独立来源") {
  const device_status::Metadata metadata{
      .device_name = "教学一号平台",
      .device_type = "教学箱",
      .dcdw_label = "一号实验台",
      .capabilities = {"telemetry"},
      .product = device::ProductInfo{"厂家", "型号"},
  };
  auto online = nlohmann::json::parse(
      device_status::BuildPayload(Box(), "2.3.4", true, metadata, "1"));
  CHECK(online.at("device_id") == Box().device_id);
  CHECK(online.at("model_version") == "2.3.4");
  CHECK(online.at("school_id") == "示例职业技术学院");
  CHECK(online.at("device_name") == "教学一号平台");
  CHECK(online.at("device_type") == "教学箱");
  CHECK(online.at("dcdw_label") == "一号实验台");
  CHECK(online.at("capabilities") == nlohmann::json::array({"telemetry"}));
  CHECK(online.at("product").at("model_code") == "型号");
  CHECK(online.at("device_status") == "1");
  const auto offline = nlohmann::json::parse(
      device_status::BuildPayload(Box(), "2.3.4", false, metadata, "1"));
  online["online"] = "1";
  CHECK(offline == online);
}

TEST_CASE("首次连接立即发布随后每十秒发布一次") {
  device_status::PublicationState state;
  CHECK_FALSE(state.ShouldPublish(false, 0, "payload", kStart));
  CHECK(state.ShouldPublish(true, 1, "payload", kStart));
  state.MarkPublished(1, "payload", kStart);
  CHECK_FALSE(state.ShouldPublish(true, 1, "payload", kStart + 9999ms));
  CHECK(state.ShouldPublish(true, 1, "payload", kStart + 10s));
  state.MarkPublished(1, "payload", kStart + 10s);
  CHECK_FALSE(state.ShouldPublish(true, 1, "payload", kStart + 19999ms));
  CHECK(state.ShouldPublish(true, 1, "payload", kStart + 20s));
}

TEST_CASE("连接代数变化和资料变化立即重发") {
  device_status::PublicationState state;
  state.MarkPublished(1, "old", kStart);
  CHECK_FALSE(state.ShouldPublish(false, 1, "new", kStart + 1s));
  CHECK(state.ShouldPublish(true, 2, "old", kStart + 1s));
  state.MarkPublished(2, "old", kStart + 1s);
  CHECK_FALSE(state.ShouldPublish(true, 2, "old", kStart + 2s));
  CHECK(state.ShouldPublish(true, 2, "new", kStart + 2s));
}

TEST_CASE("发布失败不推进周期或丢失重连待发消息") {
  device_status::PublicationState state;
  CHECK(state.ShouldPublish(true, 1, "payload", kStart));
  CHECK(state.ShouldPublish(true, 1, "payload", kStart + 1ms));
  state.MarkPublished(1, "payload", kStart + 1ms);
  CHECK(state.ShouldPublish(true, 2, "payload", kStart + 2ms));
  CHECK(state.ShouldPublish(true, 2, "payload", kStart + 3ms));
  state.MarkPublished(2, "payload", kStart + 3ms);
  CHECK(state.ShouldPublish(true, 2, "payload", kStart + 10003ms));
  CHECK(state.ShouldPublish(true, 2, "payload", kStart + 10004ms));
}
