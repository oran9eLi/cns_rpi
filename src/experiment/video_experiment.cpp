#include "experiment/video_experiment.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <deque>
#include <ranges>
#include <nlohmann/json.hpp>

#include "experiment/video_request.hpp"
#include "mqtt/topic.hpp"

namespace experiment {
namespace {
using Json = nlohmann::json;
using Wall = PiLinkInspector::WallClock;
using Steady = PiLinkInspector::SteadyClock;

std::string Utc(Wall::time_point time) {
  const auto seconds = std::chrono::floor<std::chrono::seconds>(time);
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(time - seconds).count();
  const auto epoch = Wall::to_time_t(seconds);
  std::tm utc{};
  gmtime_r(&epoch, &utc);
  char date[32]{};
  std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc);
  char fraction[8]{};
  std::snprintf(fraction, sizeof(fraction), ".%03dZ", static_cast<int>(milliseconds));
  return std::string(date) + fraction;
}

std::string_view Name(video::State state) {
  switch (state) {
    case video::State::kIdle: return "idle";
    case video::State::kRunning: return "running";
    case video::State::kStopping: return "stopping";
    case video::State::kCompleted: return "completed";
    case video::State::kFailed: return "failed";
  }
  return "idle";
}
std::string_view Name(video::Kind kind) {
  switch (kind) {
    case video::Kind::kProbe: return "probe";
    case video::Kind::kTest: return "test";
    case video::Kind::kPublish: return "publish";
  }
  return "probe";
}
template<class T> Json Nullable(const std::optional<T>& value) {
  return value ? Json(*value) : Json(nullptr);
}

Json Ack(const VideoRequest& request, Wall::time_point now, std::string_view error = {}) {
  Json result{{"schema_version", 1}, {"command_id", request.command_id},
      {"session_id", request.session_id}, {"action_id", request.action_id},
      {"operation", request.operation}, {"observed_at", Utc(now)},
      {"status", error.empty() ? "completed" : "rejected"}};
  if (!error.empty()) result["error_code"] = error;
  return result;
}
}

struct VideoExperiment::Impl {
  struct Entry {
    VideoRequest request;
    std::string topic;
    std::optional<Json> terminal;
    Steady::time_point terminated_at{};
    bool ready{false};
    bool cancelled{false};
  };
  std::string topic_namespace;
  bool enabled;
  video::Manager& manager;
  VideoExperiment::TerminalObserver on_terminal;
  std::deque<Entry> entries;
  std::string owner_session, run_id, stream_path;
  std::string lease_session;
  std::uint64_t highest_lease{0};

  Json Fact(const VideoRequest& request) const {
    const auto& s = manager.Current();
    Json cameras = Json::array();
    for (const auto& camera : s.cameras) cameras.push_back({{"index", camera.index}, {"model", camera.model}});
    return {{"state", Name(s.state)}, {"mode", Name(s.kind)},
        {"process_running", manager.Busy()}, {"pid", s.pid},
        {"parameters", {{"camera", s.parameters.camera}, {"width", s.parameters.width},
            {"height", s.parameters.height}, {"framerate", s.parameters.framerate},
            {"bitrate_bps", s.parameters.bitrate_bps}}},
        {"capture_frames", Nullable(s.capture_frames)}, {"encoded_frames", Nullable(s.encoded_frames)},
        {"encoded_duration_seconds", Nullable(s.encoded_duration_seconds)},
        {"actual_width", Nullable(s.actual_width)}, {"actual_height", Nullable(s.actual_height)},
        {"actual_codec", Nullable(s.actual_codec)}, {"exit_code", Nullable(s.exit_code)},
        {"error_code", s.error_code}, {"diagnostic", s.diagnostic.substr(0, 1024)},
        {"cameras", cameras}, {"run_id", run_id}, {"owner_session_id", owner_session},
        {"belongs_to_session", owner_session == request.session_id},
        {"stream_path", stream_path}, {"receiver_observed", nullptr}, {"playback_observed", nullptr}};
  }

  Publication Publish(const Entry& entry, const Json& terminal) const {
    return {.topic = entry.topic,
            .payload = terminal.dump(-1, ' ', false, Json::error_handler_t::replace),
            .qos = 2, .retain = false};
  }
  Outcome Finish(Entry& entry, Wall::time_point wall, Steady::time_point steady,
                 std::string_view error = {}, bool include_fact = false) {
    entry.terminal = Ack(entry.request, wall, error);
    if (include_fact) (*entry.terminal)["fact"] = Fact(entry.request);
    entry.terminated_at = steady;
    if (on_terminal) on_terminal(*entry.terminal);
    return {.publication = Publish(entry, *entry.terminal), .diagnostic = {}};
  }

  std::string Gate(const VideoRequest& request, const device::Binding& binding,
                   const PiLinkInspector::Sampler& sample, Wall::time_point now) const {
    if (request.device_id != binding.device_id) return "identity_unavailable";
    if (!enabled) return "video_disabled";
    if (now >= request.expiry) return "action_expired";
    const auto observation = sample ? sample() : std::nullopt;
    if (!observation || !observation->mqtt_connected ||
        observation->runtime.control_status != runtime_status::ControlStatus::kOnline) return "observation_unavailable";
    if (observation->runtime.device_id != binding.device_id ||
        observation->runtime.identity_status == runtime_status::IdentityStatus::kUnbound ||
        observation->runtime.identity_status == runtime_status::IdentityStatus::kConflict) return "identity_unavailable";
    if (manager.Busy() && owner_session != request.session_id) return "session_conflict";
    if (request.lease_version < highest_lease ||
        (request.lease_version == highest_lease && request.session_id != lease_session)) return "stale_lease";
    return {};
  }

  Outcome Execute(Entry& entry, Wall::time_point wall, Steady::time_point steady) {
    const auto& request = entry.request;
    if (request.operation == "video_status") {
      return Finish(entry, wall, steady, {}, true);
    }
    if (request.operation == "video_stop") {
      if (!manager.Busy()) return Finish(entry, wall, steady, {}, true);
      for (auto& pending : entries) {
        if (!pending.terminal && pending.request.operation != "video_stop") pending.cancelled = true;
      }
      manager.Stop(steady);
      return {};
    }
    if (manager.Busy()) return Finish(entry, wall, steady, "camera_busy");
    owner_session = request.session_id;
    run_id = request.action_id;
    stream_path = request.video.destination.stream_path;
    const auto started = manager.Start(request.video, steady);
    if (!started) return Finish(entry, wall, steady, "start_failed", true);
    if (request.operation == "video_start") return Finish(entry, wall, steady, {}, true);
    return {};
  }
};

VideoExperiment::VideoExperiment(std::string topic_namespace, bool enabled, video::Manager& manager,
                                 TerminalObserver on_terminal)
    : impl_(std::make_unique<Impl>(Impl{.topic_namespace = std::move(topic_namespace),
                                      .enabled = enabled, .manager = manager,
                                      .on_terminal = std::move(on_terminal),
                                      .entries = {}, .owner_session = {}, .run_id = {},
                                      .stream_path = {}, .lease_session = {}})) {}
VideoExperiment::~VideoExperiment() = default;

Outcome VideoExperiment::Handle(std::string_view topic, std::string_view payload,
    const std::optional<device::Binding>& binding, const PiLinkInspector::Sampler& sample,
    Wall::time_point wall, Steady::time_point steady) {
  auto& impl = *impl_;
  const auto subscription = DeviceSubscription(impl.topic_namespace, binding);
  if (!subscription || topic != subscription->first) return {.publication = std::nullopt, .diagnostic = "视频动作Topic与持久化主控箱身份不一致"};
  const auto request = ParseVideoRequest(payload);
  if (!request) return {.publication = std::nullopt, .diagnostic = request.error()};
  std::erase_if(impl.entries, [&](const Impl::Entry& entry) {
    return entry.terminal && !entry.ready && steady - entry.terminated_at > std::chrono::seconds(300);
  });
  const auto existing = std::ranges::find_if(impl.entries, [&](const Impl::Entry& entry) {
    return entry.request.action_id == request->action_id;
  });
  if (existing != impl.entries.end()) {
    if (existing->request.comparison != request->comparison) {
      return {.publication = impl.Publish(*existing, Ack(*request, wall, "duplicate_conflict")), .diagnostic = {}};
    }
    existing->request.command_id = request->command_id;
    if (!existing->terminal) return {};
    (*existing->terminal)["command_id"] = request->command_id;
    existing->ready = false;
    return {.publication = impl.Publish(*existing, *existing->terminal), .diagnostic = {}};
  }
  if (impl.entries.size() >= 128) {
    const auto oldest = std::ranges::find_if(impl.entries, [](const Impl::Entry& entry) {
      return entry.terminal && !entry.ready;
    });
    if (oldest == impl.entries.end()) return {.publication = std::nullopt, .diagnostic = "视频动作终态缓存已满"};
    impl.entries.erase(oldest);
  }
  impl.entries.push_back({.request = *request,
      .topic = mqtt::BuildExperimentAckTopic(impl.topic_namespace, binding->device_id),
      .terminal = std::nullopt});
  auto& entry = impl.entries.back();
  const auto error = impl.Gate(*request, *binding, sample, wall);
  if (!error.empty()) return impl.Finish(entry, wall, steady, error);
  impl.highest_lease = request->lease_version;
  impl.lease_session = request->session_id;
  return impl.Execute(entry, wall, steady);
}

std::optional<Publication> VideoExperiment::Tick(Wall::time_point wall,
    Steady::time_point steady, bool can_publish) {
  auto& impl = *impl_;
  impl.manager.Tick(steady);
  if (!impl.manager.Busy()) {
    for (auto& entry : impl.entries) {
      if (entry.terminal) continue;
      std::string error;
      if (entry.cancelled) error = "cancelled";
      else if (entry.request.operation != "video_stop") error = impl.manager.Current().error_code;
      (void)impl.Finish(entry, wall, steady, error, true);
      entry.ready = true;
    }
  }
  if (!can_publish) return std::nullopt;
  for (auto& entry : impl.entries) {
    if (!entry.ready) continue;
    entry.ready = false;
    return impl.Publish(entry, *entry.terminal);
  }
  return std::nullopt;
}

bool VideoExperiment::Busy() const {
  return impl_->manager.Busy() || std::ranges::any_of(impl_->entries, [](const Impl::Entry& entry) {
    return !entry.terminal || entry.ready;
  });
}
}
