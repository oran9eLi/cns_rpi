/** @file video_manager.cpp @brief Pi 视频子进程管理；POSIX argv 启动，不调用 shell。 */
#include "video/video_manager.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <spawn.h>
#include <thread>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace video {
namespace {

constexpr std::size_t kDiagnosticLimit = 8192;
constexpr std::size_t kVerificationLimit = 65536;

std::optional<std::uint64_t> FrameCount(std::string_view text) {
  std::uint64_t value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

std::optional<double> Duration(const nlohmann::json& value) {
  double seconds = 0;
  if (value.is_number()) {
    seconds = value.get<double>();
  } else if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  } else {
    return std::nullopt;
  }
  return std::isfinite(seconds) && seconds > 0 ? std::optional(seconds) : std::nullopt;
}

}  // namespace

struct Manager::Impl {
  enum class Stage { kCamera, kVerify };
  Settings settings;
  Snapshot snapshot;
  Request request;
  Stage stage{Stage::kCamera};
  pid_t child{0};
  int log_fd{-1};
  Clock::time_point deadline{}, kill_deadline{};
  bool timed_out{false};
  std::filesystem::path recording;
  std::string partial_line, verification_output;
  bool verification_overflow{false};

  explicit Impl(Settings value) : settings(std::move(value)) {}

  void RemoveRecording() {
    if (recording.empty()) return;
    std::error_code ignored;
    std::filesystem::remove(recording, ignored);
    recording.clear();
  }

  void Fail(std::string code, std::string detail) {
    snapshot.state = State::kFailed;
    snapshot.error_code = std::move(code);
    if (!detail.empty()) {
      snapshot.diagnostic += "\n" + detail;
      if (snapshot.diagnostic.size() > kDiagnosticLimit)
        snapshot.diagnostic.erase(0, snapshot.diagnostic.size() - kDiagnosticLimit);
    }
    RemoveRecording();
  }

  std::expected<std::vector<std::string>, std::string> CameraArguments() {
    std::vector<std::string> arguments{settings.executable};
    if (request.kind == Kind::kProbe) {
      arguments.push_back("--list-cameras");
      return arguments;
    }
    std::string output;
    if (request.kind == Kind::kTest) {
      std::error_code error;
      std::filesystem::create_directories(settings.work_directory, error);
      if (error) return std::unexpected(error.message());
      std::string pattern = (settings.work_directory / "test-XXXXXX.ts").string();
      const int fd = ::mkstemps(pattern.data(), 3);
      if (fd == -1) return std::unexpected(std::strerror(errno));
      ::close(fd);
      recording = pattern;
      output = pattern;
    } else {
      const auto& target = request.destination;
      output = "srt://" + target.host + ":" + std::to_string(target.port) +
               "?streamid=publish:" + target.stream_path + "&pkt_size=1316";
    }
    const auto& p = request.parameters;
    arguments.insert(arguments.end(), {"--nopreview", "--codec", "libav", "--libav-format", "mpegts",
        "--low-latency", "--profile", "baseline", "--verbose", "2", "--camera", std::to_string(p.camera),
        "--width", std::to_string(p.width), "--height", std::to_string(p.height),
        "--framerate", std::to_string(p.framerate), "--bitrate", std::to_string(p.bitrate_bps),
        "--timeout", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(request.duration).count()),
        "--output", output});
    return arguments;
  }

  std::expected<void, std::string> Spawn(const std::vector<std::string>& arguments) {
    int pipe_fds[2];
    if (::pipe2(pipe_fds, O_CLOEXEC) == -1) return std::unexpected(std::strerror(errno));
    const int flags = ::fcntl(pipe_fds[0], F_GETFL);
    if (flags == -1 || ::fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) == -1) {
      const std::string error = std::strerror(errno);
      ::close(pipe_fds[0]);
      ::close(pipe_fds[1]);
      return std::unexpected(error);
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int error = ::posix_spawn_file_actions_init(&actions);
    if (error != 0) {
      ::close(pipe_fds[0]);
      ::close(pipe_fds[1]);
      return std::unexpected(std::strerror(error));
    }
    error = ::posix_spawnattr_init(&attributes);
    if (error != 0) {
      ::posix_spawn_file_actions_destroy(&actions);
      ::close(pipe_fds[0]);
      ::close(pipe_fds[1]);
      return std::unexpected(std::strerror(error));
    }
    // 独立进程组使停止操作覆盖编码器派生进程，不影响主程序。
    error = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    if (!error) error = ::posix_spawnattr_setpgroup(&attributes, 0);
    if (!error) error = ::posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    if (!error) error = ::posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    if (!error) error = ::posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    if (!error) error = ::posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    // 主程序的 UART 锁和 MQTT socket 不能被摄像头进程继承。
    if (!error) error = ::posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (!error) error = ::posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::posix_spawnattr_destroy(&attributes);
    ::close(pipe_fds[1]);
    if (error != 0) {
      ::close(pipe_fds[0]);
      return std::unexpected(std::strerror(error));
    }
    child = pid;
    snapshot.pid = pid;
    snapshot.exit_code.reset();
    log_fd = pipe_fds[0];
    partial_line.clear();
    return {};
  }

  void ParseLine(std::string_view line) {
    if (stage != Stage::kCamera) return;
    constexpr std::string_view marker = "Viewfinder frame ";
    if (const auto position = line.find(marker); position != std::string_view::npos) {
      const auto start = position + marker.size();
      const auto end = line.find_first_not_of("0123456789", start);
      const auto frame = FrameCount(line.substr(start, end - start));
      if (frame) snapshot.capture_frames = frame;
    }
    if (request.kind != Kind::kProbe) return;
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return;
    line.remove_prefix(first);
    int index = 0;
    const auto [number_end, error] = std::from_chars(line.data(), line.data() + line.size(), index);
    if (error != std::errc{} || index < 0) return;
    line.remove_prefix(static_cast<std::size_t>(number_end - line.data()));
    const auto colon = line.find_first_not_of(" \t");
    if (colon == std::string_view::npos || line[colon] != ':') return;
    line.remove_prefix(colon + 1);
    const auto model_start = line.find_first_not_of(" \t");
    if (model_start == std::string_view::npos) return;
    line.remove_prefix(model_start);
    const auto model_end = line.find_first_of(" \t[\r");
    if (std::none_of(snapshot.cameras.begin(), snapshot.cameras.end(),
                     [index](const Camera& camera) { return camera.index == index; }))
      snapshot.cameras.push_back({index, std::string(line.substr(0, model_end))});
  }

  void Drain() {
    std::array<char, 4096> buffer{};
    // 单 Tick 最多 64 KiB；持续输出不能独占负责 UART/MQTT 的主循环。
    for (int read_count = 0; read_count < 16 && log_fd >= 0; ++read_count) {
      const auto size = ::read(log_fd, buffer.data(), buffer.size());
      if (size < 0 && errno == EINTR) continue;
      if (size <= 0) break;
      const std::string_view bytes(buffer.data(), static_cast<std::size_t>(size));
      snapshot.diagnostic.append(bytes);
      if (snapshot.diagnostic.size() > kDiagnosticLimit)
        snapshot.diagnostic.erase(0, snapshot.diagnostic.size() - kDiagnosticLimit);
      if (stage == Stage::kVerify) {
        if (verification_output.size() + bytes.size() <= kVerificationLimit)
          verification_output.append(bytes);
        else verification_overflow = true;
        continue;
      }
      partial_line.append(bytes);
      std::size_t start = 0;
      for (auto end = partial_line.find('\n'); end != std::string::npos;
           end = partial_line.find('\n', start)) {
        ParseLine(std::string_view(partial_line).substr(start, end - start));
        start = end + 1;
      }
      partial_line.erase(0, start);
      if (partial_line.size() > kDiagnosticLimit)
        partial_line.erase(0, partial_line.size() - kDiagnosticLimit);
    }
  }

  void BeginStop(Clock::time_point now, bool timeout) {
    if (child == 0 || snapshot.state == State::kStopping) return;
    timed_out = timeout;
    snapshot.state = State::kStopping;
    kill_deadline = now + std::chrono::seconds(2);
    ::kill(-child, SIGTERM);
  }

  void Verify() {
    const auto document = nlohmann::json::parse(verification_output, nullptr, false);
    if (verification_overflow || !document.is_object() || !document.contains("streams") ||
        !document["streams"].is_array() || document["streams"].empty()) {
      Fail("invalid_video", "ffprobe returned no valid video stream");
      return;
    }
    const auto& stream = document["streams"][0];
    if (!stream.is_object() || !stream.contains("codec_name") ||
        stream["codec_name"] != "h264" || !stream.contains("width") ||
        stream["width"] != request.parameters.width || !stream.contains("height") ||
        stream["height"] != request.parameters.height || !stream.contains("nb_read_frames") ||
        !stream["nb_read_frames"].is_string()) {
      Fail("invalid_video", "Expected H.264 video at requested dimensions with counted frames");
      return;
    }
    const auto frames = FrameCount(stream["nb_read_frames"].get_ref<const std::string&>());
    if (!frames || *frames == 0) {
      Fail("invalid_video", "ffprobe did not verify positive encoded frames");
      return;
    }
    snapshot.encoded_frames = frames;
    snapshot.actual_width = request.parameters.width;
    snapshot.actual_height = request.parameters.height;
    snapshot.actual_codec = "h264";
    if (stream.contains("duration")) snapshot.encoded_duration_seconds = Duration(stream["duration"]);
    if (!snapshot.encoded_duration_seconds && document.contains("format") &&
        document["format"].is_object() && document["format"].contains("duration"))
      snapshot.encoded_duration_seconds = Duration(document["format"]["duration"]);
    snapshot.state = State::kCompleted;
    RemoveRecording();
  }

  void ChildExited(int status, Clock::time_point now) {
    snapshot.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (snapshot.state == State::kStopping) {
      if (timed_out) Fail("timeout", "Child exceeded its bounded execution time");
      else {
        snapshot.state = State::kCompleted;
        RemoveRecording();
      }
      return;
    }
    if (*snapshot.exit_code != 0) {
      Fail(stage == Stage::kVerify ? "verification_failed" : "process_failed", "Child exited unsuccessfully");
      return;
    }
    if (stage == Stage::kVerify) {
      Verify();
    } else if (request.kind == Kind::kProbe) {
      if (snapshot.cameras.empty()) Fail("camera_not_found", "No indexed cameras listed");
      else snapshot.state = State::kCompleted;
    } else if (request.kind == Kind::kTest) {
      stage = Stage::kVerify;
      deadline = now + std::chrono::seconds(10);
      const auto result = Spawn({settings.ffprobe, "-v", "error", "-select_streams", "v:0",
          "-count_frames", "-show_entries",
          "stream=codec_name,width,height,nb_read_frames,duration:format=duration", "-of", "json",
          recording.string()});
      if (!result) Fail("verification_failed", result.error());
    } else snapshot.state = State::kCompleted;
  }
};

Manager::Manager(Settings settings) : impl_(std::make_unique<Impl>(std::move(settings))) {}

Manager::~Manager() {
  const auto now = Clock::now();
  impl_->BeginStop(now, false);
  // 析构允许等待回收；日常 Tick/Stop 路径不等待 2 秒宽限期。
  impl_->kill_deadline = std::min(impl_->kill_deadline, now + std::chrono::seconds(2));
  while (Busy()) {
    Tick(Clock::now());
    if (Busy()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  impl_->RemoveRecording();
}

std::expected<void, std::string> Manager::Start(const Request& request, Clock::time_point now) {
  if (Busy()) return std::unexpected("camera_busy");
  auto& impl = *impl_;
  impl.RemoveRecording();
  impl.snapshot = {};
  impl.snapshot.kind = request.kind;
  impl.snapshot.parameters = request.parameters;
  impl.request = request;
  impl.stage = Impl::Stage::kCamera;
  impl.timed_out = false;
  impl.verification_output.clear();
  impl.verification_overflow = false;
  impl.deadline = now + (request.kind == Kind::kProbe ? std::chrono::seconds(10)
                                                    : request.duration + std::chrono::seconds(10));
  const auto arguments = impl.CameraArguments();
  if (!arguments) {
    impl.Fail("start_failed", arguments.error());
    return std::unexpected(arguments.error());
  }
  const auto result = impl.Spawn(*arguments);
  if (!result) {
    impl.Fail("start_failed", result.error());
    return result;
  }
  impl.snapshot.state = State::kRunning;
  return {};
}

void Manager::Tick(Clock::time_point now) {
  auto& impl = *impl_;
  if (!Busy()) return;
  impl.Drain();
  int status = 0;
  const auto pid = ::waitpid(impl.child, &status, WNOHANG);
  if (pid == impl.child) {
    // 退出后再读一次；不等待缺少换行或持有管道的派生进程。
    impl.Drain();
    impl.ParseLine(impl.partial_line);
    ::kill(-impl.child, SIGKILL);
    ::close(impl.log_fd);
    impl.log_fd = -1;
    impl.child = 0;
    impl.snapshot.pid = 0;
    impl.ChildExited(status, now);
    return;
  }
  if (pid == -1 && errno != EINTR) {
    const std::string detail = std::strerror(errno);
    ::kill(-impl.child, SIGKILL);
    ::close(impl.log_fd);
    impl.log_fd = -1;
    impl.child = 0;
    impl.snapshot.pid = 0;
    impl.Fail("process_failed", detail);
    return;
  }
  if (impl.snapshot.state == State::kStopping) {
    if (now >= impl.kill_deadline) ::kill(-impl.child, SIGKILL);
  } else if (now >= impl.deadline) impl.BeginStop(now, true);
}

void Manager::Stop(Clock::time_point now) { impl_->BeginStop(now, false); }
bool Manager::Busy() const { return impl_->child != 0; }
const Snapshot& Manager::Current() const { return impl_->snapshot; }

}  // namespace video
