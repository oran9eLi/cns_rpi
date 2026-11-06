# G2 Pi Video Preparation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Track verified steps below.

**Goal:** Add locally testable Pi camera probing, finite test capture, bounded SRT publishing, stop and status actions on the existing experiment control channel; configure this workstation's SSH key separately.

**Architecture:** Keep capture/encode/network IO in managed `rpicam-vid` child processes, poll them without blocking the main loop, and use `ffprobe` to verify the finite test output. A video experiment controller owns request validation, Session ownership, duplicate handling and ACKs; main only dispatches and drains results.

**Tech Stack:** C++23, POSIX processes/pipes, nlohmann JSON, existing MQTT QoS 2/outbox, rpicam-apps/libav, ffprobe, CMake/CTest/doctest, Ubuntu-24.04 WSL.

**Spec:** `docs/superpowers/specs/2026-11-05-g2-video-platform-experiment-design.md`, Pi-side preparation subset. The user authorized local implementation on 2026-11-05. Server/Web work, product deployment and whole-class acceptance remain subsequent work.

## Global Constraints

- Keep the F407-bound `device_id`, existing experiment Topic and schema_version 1 envelope; video never writes UART.
- No server/F407 changes and no product deployment in this task. SSH authorized_keys setup and read-only Pi inventory are authorized separately.
- Missing video config defaults to disabled; no process starts automatically.
- No arbitrary shell command in an incoming action; execute argv directly.
- Validate at the JSON/config boundary once; internal code consumes typed values.
- Preserve MQTT outbox delivery confirmation; never start an effect when it cannot retain its result.
- Streaming is explicitly bounded, and normal shutdown stops child processes.
- `rpicam-apps v1.12.0` libav/MPEG-TS cannot use `--save-pts`; do not enable per-frame metadata in that mode.
- Captured request progress, locally verified encoded frames, receiver bytes and browser decoding have distinct meanings. Unknown fields stay null. This task observes only Pi facts.
- Tests use real process management with external executable fixtures; they do not reimplement business logic.

## Review Focus

- A missing executable must fail, rather than report successful video output.
- Same action ID redelivery must not start a second process; conflicting contents must be rejected.
- A wrong Session cannot stop another Session's camera; stale requests cannot mutate the current run.
- Stop, duration expiry and parent shutdown must reap children, including when normal output is incomplete.
- A running capture must not be reported as encoded/received/played video; finite test verification must reject invalid/no-frame output.

## File Structure

- `src/video/video_manager.hpp/.cpp`: child IO, argv, camera listing, capture progress, finite test verification and lifecycle.
- `src/experiment/video_request.hpp/.cpp`: single JSON boundary for five typed video operations.
- `src/experiment/video_experiment.hpp/.cpp`: Session ownership, controller and ACKs.
- `src/config/app_config.hpp/.cpp`, `config/config.example.json`: optional video enable/executable/ffprobe/work-directory configuration.
- `src/main.cpp`, `CMakeLists.txt`: composition, dispatch, polling and build registration.
- `tests/test_video_manager.cpp`, `tests/test_video_request.cpp`, `tests/test_video_experiment.cpp`, `tests/test_app_config.cpp`: actual implementation tests.
- `scripts/install_deps.sh`: ffmpeg package supplies finite-test ffprobe and real-media test fixture generation.
- `docs/G2树莓派视频实验接口与本地验证.md`, `docs/V1设计文档.md`, `docs/change_history/2026-11-05_G2视频实验准备.md`: exact implemented contract, limits and verification.

## Task 1: Camera process manager

**Interfaces:** namespace `video`.

```cpp
using Clock = std::chrono::steady_clock;
enum class Kind { kProbe, kTest, kPublish };
enum class State { kIdle, kRunning, kStopping, kCompleted, kFailed };
struct Parameters { int camera{0}, width{1280}, height{720}, framerate{25}, bitrate_bps{2000000}; };
struct Destination { std::string host; int port{8890}; std::string stream_path; };
struct Request { Kind kind{Kind::kProbe}; Parameters parameters; Destination destination; std::chrono::seconds duration{3}; };
struct Camera { int index; std::string model; };
struct Settings { std::string executable{"/usr/bin/rpicam-vid"}; std::string ffprobe{"/usr/bin/ffprobe"}; std::filesystem::path work_directory{"/tmp/cns-rpi-video"}; };
struct Snapshot {
  State state{State::kIdle}; Kind kind{Kind::kProbe}; Parameters parameters;
  int pid{0}; std::vector<Camera> cameras;
  std::optional<std::uint64_t> capture_frames, encoded_frames;
  std::optional<double> encoded_duration_seconds;
  std::optional<int> actual_width, actual_height;
  std::optional<std::string> actual_codec;
  std::optional<int> exit_code;
  std::string error_code, diagnostic;
};
class Manager {
 public:
  explicit Manager(Settings settings);
  ~Manager();
  std::expected<void, std::string> Start(const Request&, Clock::time_point);
  void Tick(Clock::time_point);
  void Stop(Clock::time_point);
  bool Busy() const;
  const Snapshot& Current() const;
  // implementation may use a private pimpl; no copy/move required
};
```

- [x] Write and run failing tests for probe parsing, real child startup failure, one-camera exclusion, finite capture+ffprobe validation, malformed/no-frame output, progress semantics, timeout and stop/reap.
- [x] Implement argv with no shell: `--nopreview --codec libav --libav-format mpegts --low-latency --profile baseline --verbose 2`, selected numeric parameters, duration, and output destination. Probe uses `--list-cameras`. No metadata or save-pts flags.
- [x] Publish destination uses `srt://HOST:PORT?streamid=publish:PATH&pkt_size=1316`; bounded timeout follows requested duration plus 10 seconds. Probe timeout is 10 seconds. Stop uses SIGTERM then SIGKILL after 2 seconds without blocking Tick.
- [x] Probe parses actual indexed camera lines; test writes a unique temporary TS file, then asynchronously runs ffprobe with count_frames and JSON output. Successful test requires H.264, positive frame count and matching width/height. Clean only owned files.
- [x] Parse `Viewfinder frame N` as capture_frames only. Bound retained log/diagnostic data; continuously drain pipe so verbose child output cannot wedge the main loop.
- [x] Run standalone real manager tests under WSL; root later integrates them with CMake. Report commands and results.

## Task 2: Video experiment controller and opt-in integration

**Consumes:** Task 1 Manager interface. **Produces:** `experiment::VideoExperiment(ns, enabled, manager)` with `Handle(topic,payload,binding,sampler,wall,steady) -> Outcome`, and `Tick(wall,steady,can_publish) -> optional<Publication>`. `IsVideoOperation(payload)` recognizes only this module's operations, leaving old fallbacks intact.

- [x] Write and run failing protocol tests for the existing envelope, bad ranges/types/unknown keys, expired request, wrong target, disabled feature, cached binding with offline F407 business, redelivery/conflict and Session stop ownership.
- [x] Add `video_probe`, `video_test`, `video_start`, `video_stop`, `video_status`. Use only completed/rejected business ACK states. Probe/test/stop finish asynchronously; no invented terminal success before results. Start completion only means process created, with facts explicitly showing current observations.
- [x] `video_test`/`video_start` require `parameters={camera,width,height,framerate,bitrate_bps,duration_seconds}`. Accept camera 0..3, even width 64..1920, even height 64..1080, fps 1..30, bitrate 100000..8000000; test duration 1..10 seconds, publish duration 1..1800 seconds. Start additionally requires `destination={host,port,stream_path}` with DNS/IPv4 hostname, port 1..65535 and safe nonempty relative path <=128 characters. Other actions have no added fields.
- [x] Preserve command_id/request_id/action_id/Session/lease/expires_at rules. Runtime duplicate cache is bounded to 128 actions and 300 seconds, ignoring only command_id for same-action retransmission. Same pending action updates reply command_id; conflicting payload rejects without touching the camera.
- [x] Busy camera rejects additional starts. Stop/status respect current Session and lease; an explicit stop may cancel pending probe/test, preserving both results. Do not add persistent video journals or change identity policy.
- [x] Add video config `enabled=false`, `executable`, `ffprobe`, `work_directory`; absolute nonempty paths, old configs continue loading. Add controller before Pi inspector fallback, poll every loop, retain results while outbox full; graceful destruction stops owned media children.
- [x] Register sources/tests in CMake, run focused tests, then full suite including isolated broker checks where available.

## Task 3: Documentation and final verification

- [x] Document the five operations, exact wire examples, nullable facts and enabled=false compatibility. Mark this as local preparation; receiver/browser evidence is not implemented here.
- [x] Add V1 pointer and dated change history. Record actual test totals and any skipped broker/hardware checks.
- [x] Run warning-clean build, full CTest, diff whitespace check and a fresh read-only whole-change review; fix material findings and rerun relevant checks.
- [x] Leave local changes reviewable on `codex/g2-video-preparation`, without push/deployment. Do not make unsolicited release/merge decisions.

## Execution Record

- Baseline: Ubuntu-24.04 WSL build succeeded; CTest 54/54 passed. Real-broker optional cases were not enabled in that baseline.
- SSH: public key added preserving existing authorized_keys; native Windows `ssh -o BatchMode=yes cns-pi hostname` returned raspberrypi. Pi camera inventory confirmed imx708_wide, rpicam-apps 1.12.0, ffprobe, service active. Product source not uploaded.
- Execution uses the existing user checkout on a separate feature branch so requested local edits remain visible. Independent process-manager work and control integration have disjoint file ownership.
- Config RED: five invalid video config variants were accepted before implementation; GREEN: app_config 23 cases / 197 assertions passed.
- Request RED: six cases exposed absent recognition/validation; GREEN: 6 cases / 49 assertions passed. Split boundary parsing from asynchronous control to keep each module readable.
- Controller RED: 5/5 cases failed against a nonfunctional stub; GREEN: 5 cases / 84 assertions passed with actual child processes, ownership, duplicate replay and ACK backpressure.
- Whole-change review found reproducible UART descriptor inheritance, cross-Session stale leases, a full-outbox main-loop skip, mismatched failed-run ownership and config restart interrupting pending work. Added descriptor/lease/run-ownership regressions (RED reproduced) and fixed all five. A strict-UTC digit issue was also fixed. Current standalone -Wall -Wextra -Werror checks: manager 13 cases / 189 assertions; controller 9 cases / 138 assertions.
- Fresh reviewer rechecked the final sources and tests: all five Important and one Minor findings closed, no remaining blocking finding. Final integrated evidence follows.

- Final integrated build: Ubuntu-24.04 WSL / x86_64, -Wall -Wextra, no compiler warnings; CTest 57/57 passed in 35.27 seconds with real isolated broker traffic enabled. Separate full MQTT test enabled both broker environment cases: 5 cases / 310 assertions passed, including broker stop, reconnect and restored QoS 2 delivery. Initial fixed reconnect port was occupied; retry used dynamic isolated ports and required no product-code change.
- Final whitespace checks: tracked diff and 12 new files passed. Native key-only SSH reconfirmed raspberrypi / aarch64 / cns-rpi.service active. No ARM64 build, product upload, target runtime acceptance, commit or push performed.

## Follow-up deployment authorized by the user

The execution record above describes the initial local-only stage. The user subsequently requested deployment, field configuration changes, and removal of the obsolete read-only-volume arrangement.

- Deployed the exact local source snapshot to the Pi, built natively for ARM64, and passed 9 focused CTest targets. Five pre-existing timestamp-format truncation warnings remain; this native build is not warning-free.
- Backed up the original runtime and configuration, retired the Pi's remaining old config mount service, restored its contents to an ordinary writable directory, and enabled `video` without changing MQTT credentials, serial, or cellular settings.
- Switched the main binary and verified service/watchdog operation. Real Manager camera capture produced 65 H.264 frames; localhost SRT reception produced 66 frames. Both were 1280x720.
- The Pi has no real F407 binding or serial endpoint at this time. Production MQTT video commands and server/browser/cloud acceptance remain unverified. No commit or push was performed.
- Detailed provenance, hashes, backup locations, native commands and evidence: [Pi deployment and camera verification](../../2026-11-05-G2树莓派部署与实拍验证.md).
