#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "common/mavlink.h"

namespace command_feedback {

enum class Channel { kConfig, kControl, kExperiment };
enum class Result : std::uint8_t { kFailure = 0, kSuccess = 1 };

mavlink_message_t Encode(Result result, std::uint8_t source_system,
                         std::uint8_t source_component, std::uint8_t target_system,
                         std::uint8_t target_component);

class ResultFeedback {
 public:
  // 调用方传入已生成的内部 ACK；返回值只表示应提示，不证明 F407 已接收。
  std::optional<Result> Observe(std::string_view device_id, Channel channel,
                                const nlohmann::json& ack);

 private:
  struct Entry {
    Channel channel;
    std::string id;
    Result result;
  };
  std::string device_id_;
  std::deque<Entry> recent_;
};

}  // namespace command_feedback
