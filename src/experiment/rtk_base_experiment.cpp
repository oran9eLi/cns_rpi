/** @file rtk_base_experiment.cpp @brief RTK 动作入口门禁、有界去重及异步配置回执。 */
#include "experiment/rtk_base_experiment.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace experiment {
namespace {
using Json = nlohmann::json;
using Wall = std::chrono::system_clock;

std::string Utc(Wall::time_point now) {
  const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
  const auto epoch = Wall::to_time_t(seconds);
  std::tm utc{};
  gmtime_r(&epoch, &utc);
  char date[32]{}, fraction[8]{};
  std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
  std::snprintf(fraction, sizeof(fraction), ".%03dZ", static_cast<int>(ms));
  return std::string(date) + fraction;
}

Json Ack(const RtkBaseRequest& request, Wall::time_point now,
         std::string_view status, const Json& base, std::string_view error = {}) {
  Json value{{"schema_version", 1}, {"command_id", request.command_id},
      {"session_id", request.session_id}, {"action_id", request.action_id},
      {"operation", request.operation}, {"observed_at", Utc(now)},
      {"status", status}, {"fact", {{request.operation.starts_with("rtk_rover_")
                                        ? "rtk" : "base", base}}}};
  if (!error.empty()) value["error_code"] = error;
  return value;
}
}

RtkBaseExperiment::RtkBaseExperiment(std::string topic_namespace)
    : topic_namespace_(std::move(topic_namespace)) {}

Publication RtkBaseExperiment::Publish(const Entry& entry) const {
  return {.topic = topic_namespace_ + "/" + entry.request.device_id + "/experiment/ack",
      .payload = entry.acknowledgement.dump(-1, ' ', false, Json::error_handler_t::replace),
      .qos = 2, .retain = false};
}

Outcome RtkBaseExperiment::Handle(std::string_view topic, std::string_view payload,
                                 const std::optional<std::string>& identity,
                                 const Sampler& sample, const Starter& start,
                                 WallClock::time_point now, const Operator& operate,
                                 const Sampler& rover_sample) {
  auto request = ParseRtkBaseRequest(payload);
  if (!request) return {.publication = {}, .diagnostic = request.error()};
  if (!identity || topic != topic_namespace_ + "/" + *identity + "/experiment/set")
    return {.publication = {}, .diagnostic = "RTK请求没有已绑定箱子身份或Topic不匹配"};

  Entry entry{*request, {}, false, now};
  const bool rover = request->operation.starts_with("rtk_rover_");
  const auto& observe = rover && rover_sample ? rover_sample : sample;
  const auto reject = [&](std::string_view error) -> Outcome {
    entry.request.device_id = *identity;
    entry.acknowledgement = Ack(*request, now, "rejected", observe(), error);
    return {.publication = Publish(entry), .diagnostic = {}};
  };
  if (request->device_id != *identity) return reject("wrong_target");
  const auto previous = std::find_if(entries_.begin(), entries_.end(), [&](const auto& item) {
    return item.request.action_id == request->action_id;
  });
  if (previous != entries_.end()) {
    if (previous->request.comparison != request->comparison) return reject("action_conflict");
    auto repeat = *previous;
    repeat.acknowledgement["command_id"] = request->command_id;
    return {.publication = Publish(repeat), .diagnostic = {}};
  }
  if (request->expiry <= now) return reject("expired");
  if (request->lease_version < highest_lease_ ||
      (request->lease_version == highest_lease_ && lease_session_ != request->session_id))
    return reject("stale_lease");
  std::erase_if(entries_, [&](const auto& item) {
    return !item.pending && now - item.completed_at >= std::chrono::seconds{300};
  });
  if (entries_.size() >= 128) return reject("action_cache_full");
  const auto complete = [&](std::string_view status, std::string_view error = {}) -> Outcome {
    highest_lease_ = request->lease_version;
    lease_session_ = request->session_id;
    entry.acknowledgement = Ack(*request, now, status, observe(), error);
    entries_.push_back(std::move(entry));
    return {.publication = Publish(entries_.back()), .diagnostic = {}};
  };
  if (rover) {
    if (!rover_sample || (!operate && request->operation != "rtk_rover_status"))
      return complete("rejected", "rtk_rover_unavailable");
    if (request->operation != "rtk_rover_status") {
      const auto result = operate(request->operation);
      if (!result) {
        auto outcome = complete("rejected", "rtk_rover_rejected");
        outcome.diagnostic = result.error();
        return outcome;
      }
    }
  }
  if (request->operation == "rtk_base_survey_in") {
    if (std::any_of(entries_.begin(), entries_.end(), [](const auto& item) { return item.pending; }))
      return reject("configuration_busy");
    const auto result = start(request->minimum_duration_s, request->accuracy_limit_m);
    if (!result) {
      auto outcome = reject("base_configuration_rejected");
      outcome.diagnostic = result.error();
      return outcome;
    }
    entry.pending = true;
  }
  return complete(entry.pending ? "accepted" : "completed");
}

std::optional<Publication> RtkBaseExperiment::Tick(
    const Sampler& sample, const Canceller& cancel, WallClock::time_point now) {
  for (auto& entry : entries_) {
    if (!entry.pending) continue;
    auto base = sample();
    const auto configuration = base.value("configuration", Json::object());
    const auto state = configuration.value("state", std::string{});
    std::string error;
    if (entry.request.expiry <= now) {
      cancel();
      base = sample();
      error = "configuration_timeout";
    } else if (state == "failed") error = "base_configuration_failed";
    else if (!base.value("connected", false)) error = "base_disconnected";
    else if (state != "verified") continue;
    entry.pending = false;
    entry.completed_at = now;
    entry.acknowledgement = Ack(entry.request, now, error.empty() ? "completed" : "failed", base, error);
    return Publish(entry);
  }
  return {};
}

}  // namespace experiment
