#include "telemetry/publisher.hpp"

#include <cstddef>
#include <utility>

namespace telemetry {

Publisher::Publisher(std::vector<ChannelDefinition> channels,
                     Clock::time_point started_at) {
  channels_.reserve(channels.size());
  for (auto& channel : channels) {
    channels_.push_back(ChannelState{
        .definition = std::move(channel),
        .last_attempt = started_at,
    });
  }
}

std::vector<PublishAttempt> Publisher::Tick(
    Clock::time_point now, const SnapshotProvider& snapshot_provider,
    const PublishFunction& publish) {
  std::vector<std::size_t> due;
  due.reserve(channels_.size());
  for (std::size_t index = 0; index < channels_.size(); ++index) {
    const auto& channel = channels_[index];
    if (channel.definition.options.enabled &&
        now - channel.last_attempt >= channel.definition.options.interval) {
      due.push_back(index);
    }
  }
  if (due.empty()) {
    return {};
  }

  const auto snapshot = snapshot_provider();
  std::vector<PublishAttempt> attempts;
  attempts.reserve(due.size());
  for (const auto index : due) {
    auto& channel = channels_[index];
    const auto& definition = channel.definition;
    const auto frame = definition.build_frame(snapshot, channel.sequence).dump();
    const bool succeeded = publish(
        definition.options.topic, frame, definition.options.qos,
        definition.options.retain);
    const bool should_log_failure = !succeeded && !channel.failure_active;
    channel.last_attempt = now;
    ++channel.sequence;
    channel.failure_active = !succeeded;
    attempts.push_back(PublishAttempt{
        .channel_name = definition.options.name,
        .succeeded = succeeded,
        .should_log_failure = should_log_failure,
    });
  }
  return attempts;
}

void Publisher::Reset(Clock::time_point now) {
  for (auto& channel : channels_) {
    channel.last_attempt = now;
    channel.sequence = 0;
    channel.failure_active = false;
  }
}

}  // namespace telemetry
