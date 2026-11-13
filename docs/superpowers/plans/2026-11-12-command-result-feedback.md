# Pi Command Result Feedback Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans in the existing isolated worktree. Implement and verify the tasks sequentially.

**Goal:** Send success/failure notifications to the current F407 after each command reaches its final result.

**Architecture:** A small feedback module maps existing terminal ACKs, suppresses repeated results and encodes MAVLink TUNNEL notifications. Main reuses the existing serial writer at config/control result publication and experiment result enqueue points.

**Tech Stack:** C++23, vendored MAVLink headers, doctest, Python unittest, PTY, Mosquitto.

**Spec:** `docs/superpowers/specs/2026-11-12-command-result-feedback-design.md`

## Global Constraints

- TUNNEL `payload_type=0x8040`, logical payload length 2, bytes `01 01` success / `01 00` failure.
- Only final results; reuse current dynamic endpoint and sole UART owner.
- No F407 firmware, generated-header edits, push or deployment.
- Preserve MQTT ACK formats and existing execution/transaction behavior.

## Review Focus

- An asynchronous control operation must not prompt before its final ACK.
- ACK retries and repeated experiment delivery must not repeatedly prompt.
- Failed notifications can have trailing zero bytes removed on the MAVLink wire.
- Invalid parameters and command timeouts must produce failure, not success.
- An unavailable or unverified F407 endpoint must not receive a deferred old prompt.

### Task 1: Reproduce missing feedback through the actual gateway

**Files:** `tests/test_command_feedback_gateway.py`, `tests/command_feedback_fixture.cpp`, `CMakeLists.txt`.

- [x] Add a PTY/Broker integration test using the existing GNSS fixture as a real F407 input stream and a fixture using the official MAVLink decoder for Pi output.
- [x] Assert a successful hot config command emits TUNNEL `0x8040`, length 2, version 1, result 1; run against the unchanged app and record the missing-frame failure.

### Task 2: Implement feedback and command integration

**Files:** `src/command_feedback/result_feedback.hpp`, `src/command_feedback/result_feedback.cpp`, `tests/test_command_feedback.cpp`, `src/main.cpp`, `CMakeLists.txt`.

**Interfaces:** `Channel`/`Result` enums, `ResultFeedback::Observe(device_id, channel, terminal_ack) -> optional<Result>`, and `Encode(result, source_system, source_component, target_system, target_component) -> mavlink_message_t`.

- [x] Write mapping, repetition, identity and wire-roundtrip tests with the exact protocol values.
- [x] Implement the two-byte encoder and bounded in-memory result history.
- [x] Wire experiment result enqueue, config completion and all control ACK paths to one serial feedback emitter; handle invalid experiment requests as failure and ignore progress.
- [x] Extend the process test to exercise configuration failure, experiment success/rejection/retry, control progress/success/rejection/timeout, and malformed requests.
- [x] Run targeted tests and verify the earlier process test now passes.

### Task 3: Document and verify

**Files:** `docs/Pi命令结果提示与F407接口.md`, `docs/V1设计文档.md`.

- [x] Document the exact envelope, two payload examples, mappings, timing, duplicate behavior and F407 receiver requirements.
- [x] Review the final diff and run a Release build plus `scripts/ci/run_tests.py` (all CTest targets and cellular checks).
- [x] Report actual software verification and separate firmware/hardware work still pending.
