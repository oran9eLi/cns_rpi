/** @file test_video_manager.cpp @brief 通过真实子进程和外部摄像头夹具验证进程边界。 */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "video/video_manager.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path executable;

  explicit Fixture(const std::string& body) {
    char path[] = "/tmp/cns-video-manager-test-XXXXXX";
    const auto created = ::mkdtemp(path);
    if (!created) throw std::runtime_error("mkdtemp failed");
    directory = created;
    executable = directory / "camera.py";
    std::ofstream script(executable);
    script << "#!/usr/bin/python3\n"
              "import os, signal, subprocess, sys, time\n"
              "from pathlib import Path\n"
              "Path(__file__).with_suffix('.args').write_text('\\n'.join(sys.argv[1:]))\n"
           << body;
    script.close();
    std::filesystem::permissions(executable, std::filesystem::perms::owner_all);
  }
  ~Fixture() { std::filesystem::remove_all(directory); }
  video::Settings Settings() const {
    return {executable.string(), "/usr/bin/ffprobe", directory / "work"};
  }
  std::string Arguments() const {
    std::ifstream file(directory / "camera.args");
    return {std::istreambuf_iterator<char>(file), {}};
  }
};

bool Finish(video::Manager& manager, video::Clock::time_point now,
            std::chrono::milliseconds limit = 5s) {
  const auto deadline = video::Clock::now() + limit;
  while (manager.Busy() && video::Clock::now() < deadline) {
    manager.Tick(now);
    std::this_thread::sleep_for(2ms);
  }
  return !manager.Busy();
}

bool WaitFor(const std::function<bool()>& condition, video::Manager& manager,
             video::Clock::time_point now) {
  const auto deadline = video::Clock::now() + 3s;
  while (!condition() && video::Clock::now() < deadline) {
    manager.Tick(now);
    std::this_thread::sleep_for(2ms);
  }
  return condition();
}

video::Request Publish() {
  video::Request request;
  request.kind = video::Kind::kPublish;
  request.parameters = {2, 320, 240, 10, 400000};
  request.destination = {"127.0.0.1", 8890, "student/camera"};
  request.duration = 4s;
  return request;
}

void Reaped(int pid) {
  int status = 0;
  CHECK(::waitpid(pid, &status, WNOHANG) == -1);
  CHECK(errno == ECHILD);
  CHECK(::kill(pid, 0) == -1);
  CHECK(errno == ESRCH);
}

}  // namespace

TEST_CASE("probe parses indexed camera lines and reaps its child") {
  Fixture fixture("print('Available cameras\\n-----------------\\n'\n"
                  "'0 : imx708_wide [4608x2592 10-bit RGGB] (/base/soc/i2c)\\n'\n"
                  "'    Modes: 2304x1296\\n'\n"
                  "'2 : imx219 [3280x2464 10-bit RGGB] (/base/soc/i2c)')\n");
  video::Manager manager(fixture.Settings());
  const auto now = video::Clock::now();
  REQUIRE(manager.Start({}, now));
  const int pid = manager.Current().pid;
  REQUIRE(Finish(manager, now));
  CHECK(manager.Current().state == video::State::kCompleted);
  REQUIRE(manager.Current().cameras.size() == 2);
  CHECK(manager.Current().cameras[0].index == 0);
  CHECK(manager.Current().cameras[0].model == "imx708_wide");
  CHECK(manager.Current().cameras[1].index == 2);
  CHECK(fixture.Arguments() == "--list-cameras");
  CHECK_FALSE(manager.Current().encoded_frames);
  Reaped(pid);
}

TEST_CASE("missing or non-executable program fails at Start") {
  Fixture fixture("pass\n");
  auto settings = fixture.Settings();
  settings.executable = (fixture.directory / "absent").string();
  video::Manager missing(settings);
  CHECK_FALSE(missing.Start({}, video::Clock::now()));
  CHECK(missing.Current().state == video::State::kFailed);
  CHECK(missing.Current().error_code == "start_failed");
  CHECK_FALSE(missing.Busy());
  std::filesystem::permissions(fixture.executable, std::filesystem::perms::owner_read);
  video::Manager denied(fixture.Settings());
  CHECK_FALSE(denied.Start({}, video::Clock::now()));
}

TEST_CASE("camera child does not inherit parent device descriptors") {
  const int descriptor = ::dup(STDOUT_FILENO);
  REQUIRE(descriptor >= 3);
  Fixture fixture("try:\n    os.fstat(" + std::to_string(descriptor) +
      ")\nexcept OSError:\n    print('0 : imx708_wide [4608x2592]')\nelse:\n    sys.exit(19)\n");
  video::Manager manager(fixture.Settings());
  const auto now = video::Clock::now();
  const auto started = manager.Start({}, now);
  ::close(descriptor);
  REQUIRE(started);
  REQUIRE(Finish(manager, now));
  CHECK(manager.Current().state == video::State::kCompleted);
}

TEST_CASE("one camera remains owned until stopped and capture progress is not encoded video") {
  Fixture fixture("print('Viewfinder frame 17', file=sys.stderr, flush=True)\ntime.sleep(30)\n");
  video::Manager manager(fixture.Settings());
  const auto now = video::Clock::now();
  REQUIRE(manager.Start(Publish(), now));
  const int pid = manager.Current().pid;
  REQUIRE(WaitFor([&] { return manager.Current().capture_frames.has_value(); }, manager, now));
  CHECK(manager.Current().capture_frames == 17);
  CHECK_FALSE(manager.Current().encoded_frames);
  CHECK_FALSE(manager.Current().encoded_duration_seconds);
  CHECK_FALSE(manager.Current().actual_codec);
  CHECK_FALSE(manager.Start({}, now));
  CHECK(manager.Current().pid == pid);
  const std::string args = fixture.Arguments();
  CHECK(args.find("--nopreview\n--codec\nlibav\n--libav-format\nmpegts\n") != std::string::npos);
  CHECK(args.find("--low-latency\n--profile\nbaseline\n--verbose\n2\n") != std::string::npos);
  CHECK(args.find("--camera\n2\n--width\n320\n--height\n240\n--framerate\n10\n--bitrate\n400000\n--timeout\n4000\n") != std::string::npos);
  CHECK(args.find("srt://127.0.0.1:8890?streamid=publish:student/camera&pkt_size=1316") != std::string::npos);
  CHECK(args.find("--save-pts") == std::string::npos);
  CHECK(args.find("--metadata") == std::string::npos);
  manager.Stop(now);
  CHECK(manager.Current().state == video::State::kStopping);
  REQUIRE(Finish(manager, now));
  CHECK(manager.Current().state == video::State::kCompleted);
  Reaped(pid);
  REQUIRE(manager.Start({}, now));
  manager.Stop(now);
  REQUIRE(Finish(manager, now));
}

TEST_CASE("finite test uses real ffprobe and removes only its own recording") {
  Fixture fixture("output = sys.argv[sys.argv.index('--output') + 1]\n"
                  "subprocess.run(['/usr/bin/ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', "
                  "'color=c=black:size=320x240:rate=10', '-t', '1', '-c:v', 'libx264', "
                  "'-profile:v', 'baseline', '-f', 'mpegts', '-y', output], check=True)\n"
                  "print('Viewfinder frame 9', file=sys.stderr)\n");
  std::filesystem::create_directories(fixture.Settings().work_directory);
  const auto sentinel = fixture.Settings().work_directory / "unrelated.ts";
  std::ofstream(sentinel) << "keep";
  video::Manager manager(fixture.Settings());
  auto request = Publish();
  request.kind = video::Kind::kTest;
  const auto now = video::Clock::now();
  REQUIRE(manager.Start(request, now));
  REQUIRE(Finish(manager, now));
  CHECK(manager.Current().state == video::State::kCompleted);
  CHECK(manager.Current().capture_frames == 9);
  CHECK(manager.Current().encoded_frames == 10);
  CHECK(manager.Current().actual_codec == "h264");
  CHECK(manager.Current().actual_width == 320);
  CHECK(manager.Current().actual_height == 240);
  REQUIRE(manager.Current().encoded_duration_seconds.has_value());
  CHECK(*manager.Current().encoded_duration_seconds > 0);
  CHECK(std::filesystem::exists(sentinel));
  CHECK(std::distance(std::filesystem::directory_iterator(fixture.Settings().work_directory),
                      std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("invalid finite output and ffprobe startup failure cannot report encoded success") {
  Fixture fixture("output = sys.argv[sys.argv.index('--output') + 1]\n"
                  "Path(output).write_text('invalid MPEG TS')\n"
                  "print('Viewfinder frame 100', file=sys.stderr)\n");
  auto request = Publish();
  request.kind = video::Kind::kTest;
  SUBCASE("real ffprobe rejects malformed data") {
    video::Manager manager(fixture.Settings());
    const auto now = video::Clock::now();
    REQUIRE(manager.Start(request, now));
    REQUIRE(Finish(manager, now));
    CHECK(manager.Current().state == video::State::kFailed);
    CHECK_FALSE(manager.Current().encoded_frames);
  }
  SUBCASE("missing ffprobe executable") {
    auto settings = fixture.Settings();
    settings.ffprobe = (fixture.directory / "missing-ffprobe").string();
    video::Manager manager(settings);
    const auto now = video::Clock::now();
    REQUIRE(manager.Start(request, now));
    REQUIRE(Finish(manager, now));
    CHECK(manager.Current().state == video::State::kFailed);
    CHECK(manager.Current().error_code == "verification_failed");
  }
  SUBCASE("empty recording has no encoded frames") {
    Fixture empty("output = sys.argv[sys.argv.index('--output') + 1]\nPath(output).write_bytes(b'')\n");
    video::Manager manager(empty.Settings());
    const auto now = video::Clock::now();
    REQUIRE(manager.Start(request, now));
    REQUIRE(Finish(manager, now));
    CHECK(manager.Current().state == video::State::kFailed);
    CHECK_FALSE(manager.Current().encoded_frames);
  }
}

TEST_CASE("successful ffprobe exit still requires frames h264 and matching dimensions") {
  Fixture camera("output = sys.argv[sys.argv.index('--output') + 1]\nPath(output).write_text('fixture')\n");
  for (const std::string& stream : std::vector<std::string>{
           R"({"codec_name":"h264","width":320,"height":240,"nb_read_frames":"0"})",
           R"({"codec_name":"h264","width":640,"height":240,"nb_read_frames":"10"})",
           R"({"codec_name":"hevc","width":320,"height":240,"nb_read_frames":"10"})",
           R"({"codec_name":"h264","width":320,"height":240,"nb_read_frames":"N/A"})",
           std::string("null")}) {
    Fixture probe("print('" + std::string("{\"streams\":[") + stream + "]}')\n");
    auto settings = camera.Settings();
    settings.ffprobe = probe.executable.string();
    video::Manager manager(settings);
    auto request = Publish();
    request.kind = video::Kind::kTest;
    const auto now = video::Clock::now();
    REQUIRE(manager.Start(request, now));
    REQUIRE(Finish(manager, now));
    CHECK(manager.Current().state == video::State::kFailed);
    CHECK(manager.Current().error_code == "invalid_video");
    CHECK_FALSE(manager.Current().encoded_frames);
  }
}

TEST_CASE("no cameras and abnormal camera exit fail without invented output evidence") {
  Fixture fixture("print('Available cameras\\nModes: 0 : not_a_camera')\n");
  video::Manager manager(fixture.Settings());
  const auto now = video::Clock::now();
  REQUIRE(manager.Start({}, now));
  REQUIRE(Finish(manager, now));
  CHECK(manager.Current().state == video::State::kFailed);
  CHECK(manager.Current().error_code == "camera_not_found");
  Fixture broken("print('camera failure', file=sys.stderr)\nsys.exit(7)\n");
  video::Manager failed(broken.Settings());
  REQUIRE(failed.Start(Publish(), now));
  REQUIRE(Finish(failed, now));
  CHECK(failed.Current().state == video::State::kFailed);
  CHECK(failed.Current().exit_code == 7);
  CHECK(failed.Current().diagnostic.find("camera failure") != std::string::npos);
}

TEST_CASE("ffprobe remains asynchronous bounded and cancellable while owning the run") {
  Fixture camera("output = sys.argv[sys.argv.index('--output') + 1]\n"
                 "Path(output).write_text('fixture')\n"
                 "print('Viewfinder frame 7', file=sys.stderr)\n");
  Fixture probe("signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                "Path(__file__).with_suffix('.ready').write_text('ready')\n"
                "sys.stdout.write('{\"streams\":[')\nsys.stdout.flush()\ntime.sleep(30)\n");
  auto settings = camera.Settings();
  settings.ffprobe = probe.executable.string();
  video::Manager manager(settings);
  auto request = Publish();
  request.kind = video::Kind::kTest;
  const auto now = video::Clock::now();
  REQUIRE(manager.Start(request, now));
  const int camera_pid = manager.Current().pid;
  REQUIRE(WaitFor([&] { return std::filesystem::exists(probe.directory / "camera.ready"); }, manager, now));
  const int probe_pid = manager.Current().pid;
  REQUIRE(probe_pid != camera_pid);
  Reaped(camera_pid);
  CHECK(manager.Current().state == video::State::kRunning);
  CHECK(manager.Current().capture_frames == 7);
  CHECK_FALSE(manager.Current().exit_code);
  CHECK_FALSE(manager.Current().encoded_frames);
  CHECK_FALSE(manager.Start(Publish(), now));
  CHECK(probe.Arguments().find("-count_frames\n") != std::string::npos);
  CHECK(probe.Arguments().find("-of\njson\n") != std::string::npos);
  const auto before = video::Clock::now();
  SUBCASE("explicit stop cancels verification without encoded evidence") {
    manager.Stop(now);
    CHECK(video::Clock::now() - before < 100ms);
    manager.Tick(now + 2s);
    REQUIRE(Finish(manager, now + 2s));
    CHECK(manager.Current().state == video::State::kCompleted);
    CHECK(manager.Current().error_code.empty());
  }
  SUBCASE("verification watchdog stops incomplete JSON output") {
    manager.Tick(now + 10s);
    CHECK(video::Clock::now() - before < 100ms);
    CHECK(manager.Current().state == video::State::kStopping);
    manager.Tick(now + 12s);
    REQUIRE(Finish(manager, now + 12s));
    CHECK(manager.Current().state == video::State::kFailed);
    CHECK(manager.Current().error_code == "timeout");
  }
  CHECK_FALSE(manager.Current().encoded_frames);
  CHECK(std::filesystem::is_empty(settings.work_directory));
  Reaped(probe_pid);
}

TEST_CASE("duration watchdog escalates TERM to KILL without blocking and reaps child") {
  Fixture fixture("signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                  "print('Viewfinder frame 1', flush=True)\ntime.sleep(30)\n");
  video::Manager manager(fixture.Settings());
  const auto now = video::Clock::now();
  REQUIRE(manager.Start(Publish(), now));
  const int pid = manager.Current().pid;
  REQUIRE(WaitFor([&] { return manager.Current().capture_frames.has_value(); }, manager, now));
  manager.Tick(now + 13s);
  CHECK(manager.Current().state == video::State::kRunning);
  const auto before = video::Clock::now();
  manager.Tick(now + 14s);
  CHECK(video::Clock::now() - before < 100ms);
  CHECK(manager.Current().state == video::State::kStopping);
  CHECK(::kill(pid, 0) == 0);
  manager.Tick(now + 15s);
  CHECK(::kill(pid, 0) == 0);
  manager.Tick(now + 16s);
  REQUIRE(Finish(manager, now + 16s));
  CHECK(manager.Current().state == video::State::kFailed);
  CHECK(manager.Current().error_code == "timeout");
  Reaped(pid);
}

TEST_CASE("probe watchdog is ten seconds and explicit stop does not reset kill grace") {
  Fixture fixture("signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                  "print('Viewfinder frame 1', flush=True)\ntime.sleep(30)\n");
  video::Manager manager(fixture.Settings());
  const auto now = video::Clock::now();
  REQUIRE(manager.Start({}, now));
  const int pid = manager.Current().pid;
  REQUIRE(WaitFor([&] { return manager.Current().capture_frames.has_value(); }, manager, now));
  manager.Tick(now + 10s);
  CHECK(manager.Current().state == video::State::kStopping);
  manager.Stop(now + 11s);
  manager.Tick(now + 12s);
  REQUIRE(Finish(manager, now + 12s));
  CHECK(manager.Current().error_code == "timeout");
  Reaped(pid);
}

TEST_CASE("verbose output is drained with bounded diagnostics and bounded Tick latency") {
  Fixture fixture("for i in range(50000): print('x' * 200, file=sys.stderr)\n"
                  "print('Viewfinder frame 50000', flush=True)\n");
  video::Manager manager(fixture.Settings());
  const auto now = video::Clock::now();
  REQUIRE(manager.Start(Publish(), now));
  const auto deadline = video::Clock::now() + 5s;
  video::Clock::duration longest_tick{};
  while (manager.Busy() && video::Clock::now() < deadline) {
    const auto before = video::Clock::now();
    manager.Tick(now);
    longest_tick = std::max(longest_tick, video::Clock::now() - before);
    std::this_thread::sleep_for(1ms);
  }
  CHECK(longest_tick < 100ms);
  CHECK_FALSE(manager.Busy());
  CHECK(manager.Current().state == video::State::kCompleted);
  CHECK(manager.Current().capture_frames == 50000);
  CHECK(manager.Current().diagnostic.size() <= 8192);
  CHECK_FALSE(manager.Current().encoded_frames);
}

TEST_CASE("destructor stops and reaps camera even without a final output line") {
  Fixture fixture("signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                  "Path(__file__).with_suffix('.ready').write_text('ready')\ntime.sleep(30)\n");
  int pid = 0;
  const auto before = video::Clock::now();
  {
    video::Manager manager(fixture.Settings());
    const auto now = video::Clock::now();
    REQUIRE(manager.Start(Publish(), now));
    pid = manager.Current().pid;
    REQUIRE(WaitFor([&] { return std::filesystem::exists(fixture.directory / "camera.ready"); }, manager, now));
  }
  CHECK(video::Clock::now() - before < 3s);
  Reaped(pid);
}
