/**
 * @file video_manager.hpp
 * @brief Pi 本地摄像头子进程管理；不接触 UART 或 MQTT。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace video {

using Clock = std::chrono::steady_clock;
enum class Kind { kProbe, kTest, kPublish };
enum class State { kIdle, kRunning, kStopping, kCompleted, kFailed };

/** @brief 已由配置或 JSON 入口校验的采集参数；码率单位 bit/s。 */
struct Parameters {
  int camera{0}, width{1280}, height{720}, framerate{25}, bitrate_bps{2000000};
};
struct Destination {
  std::string host;
  int port{8890};
  std::string stream_path;
};
/** @brief 时长由入口限制；管理器只执行类型化请求，不接收 shell 命令。 */
struct Request {
  Kind kind{Kind::kProbe};
  Parameters parameters;
  Destination destination;
  std::chrono::seconds duration{3};
};
struct Camera { int index; std::string model; };
struct Settings {
  std::string executable{"/usr/bin/rpicam-vid"};
  std::string ffprobe{"/usr/bin/ffprobe"};
  std::filesystem::path work_directory{"/tmp/cns-rpi-video"};
};
/** @brief capture_frames 只是采集进度；encoded_* 仅来自有限文件的 ffprobe 验证。 */
struct Snapshot {
  State state{State::kIdle};
  Kind kind{Kind::kProbe};
  Parameters parameters;
  int pid{0};
  std::vector<Camera> cameras;
  std::optional<std::uint64_t> capture_frames, encoded_frames;
  std::optional<double> encoded_duration_seconds;
  std::optional<int> actual_width, actual_height;
  std::optional<std::string> actual_codec;
  std::optional<int> exit_code;
  std::string error_code, diagnostic;
};

/** @brief 仅由主循环线程调用；Tick/Stop 不等待子进程，析构负责终止并回收。 */
class Manager {
 public:
  explicit Manager(Settings settings);
  ~Manager();
  /** @brief 忙时拒绝覆盖当前任务；执行文件无法启动时返回错误。 */
  std::expected<void, std::string> Start(const Request&, Clock::time_point);
  void Tick(Clock::time_point);
  void Stop(Clock::time_point);
  bool Busy() const;
  const Snapshot& Current() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace video
