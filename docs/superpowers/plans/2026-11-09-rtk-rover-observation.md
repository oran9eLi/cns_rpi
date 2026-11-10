# RTK Rover Observation Implementation Plan

> **For agentic workers:** Use subagent-driven-development or executing-plans, with real implementation tests and an independent final review.

**Goal:** Pi-only RTCM relay and teaching observations for a web-driven RTK rover experiment.
**Architecture:** Existing unique serial owners and box identity; additive RTK telemetry and typed operations.
**Tech Stack:** C++23, official MAVLink headers, nlohmann/json, doctest, PTY, Mosquitto, WSL GCC15 / Pi ARM64 GCC14.
**Spec:** docs/superpowers/specs/2026-11-09-rtk-rover-observation-design.md

## Global Constraints

Preserve current uncommitted RTK base work on codex/rtk-base-station. Do not commit, push, alter F407, server or UI. Never invent raw bytes, delivery ACK or Fixed results. Cached box identity is allowed; PX4 must not register another business identity. Generated headers remain unchanged. Validate JSON at ingress once. Preserve real field config and 5G service if deploying.

## Review Focus

Lossless RTCM fragmentation including exact multiples; truthful successful-send evidence; GPS measurement freshness independent of heartbeat; bounded low-frequency samples; existing base operations and single-identity MQTT path.

### Task 1: RTCM frame extraction and wire encoding

Own src/rtk/base_station.hpp/.cpp, new src/rtk/rtcm_encoder.hpp/.cpp, tests/test_rtk_base_station.cpp, new tests/test_rtk_rtcm_encoder.cpp. Root integrates CMake.

Expose RtcmFrame {uint64_t sequence; steady_clock::time_point received_at; uint16_t message_type; vector<uint8_t> bytes;} and BaseStation::DrainRtcmFrames(). Queue at most 32 CRC-valid complete frames; count drops in base.rtcm.queue_dropped. Preserve original frame bytes. Include latest actual NAV-SVIN wire frame in base.survey_in.raw_hex with age inherited from existing sample metadata. Clear pending queue on disconnect. EncodeRtcm(span<const uint8_t> bytes, uint8_t& sequence) returns vector<mavlink_message_t>, sender255/MISSIONPLANNER; consume next 5-bit sequence per up-to-720-byte group. Exact 180-byte or 360/540-byte messages need empty terminal fragment; 720 uses four full fragments. Keep nonempty <=180 unfragmented. >720 goes in consecutive groups with no interleaving. Test official decode and hand-derived fragment metadata and reconstructed bytes, 180/181/360/540/720/721/1029, sequence wrap, CRC-bad exclusion and queue bound. Watch tests fail before implementation; report actual RED/GREEN evidence.

### Task 2: PX4 GPS sample freshness and student observation

Own src/rtk/px4_session.hpp/.cpp and tests/test_rtk_px4_session.cpp. Add GpsObservation(now) JSON with known, stale (3s), sample_sequence, frame_age_ms, sample_age_ms, source_time_usec, fix_type (null if stale), last_fix_type, fix_name, satellites_visible, lat_deg/lon_deg/alt_m, h_acc_m/v_acc_m (null if unknown, not supplied, no valid 3D fix), accuracy_kind=receiver_estimate, raw_hex from actual received frame. Change Observe(message,now,span<const uint8_t> wire={}) compatibly. Accepted GPS_RAW_INT source timestamp changes advance sequence and sample time; duplicates refresh frame age only; zero source time explicitly uses receive time. Invalidate clears GPS evidence. Maintain bounded 16 fix transitions with sequence and age, only new sample/changed fix. Other endpoint messages never affect it. Request GPS_RAW_INT at 1Hz via MAV_CMD_SET_MESSAGE_INTERVAL once per verified connection; also request AUTOPILOT_VERSION via REQUEST_MESSAGE and expose firmware metadata when actually received. Tests stale GPS with fresh heartbeat, duplicate source time, identity/disconnect reset, unknown accuracy, raw bytes, real transitions and outbound requests. Do not write CMake or other files.

### Task 3: Relay, operation controller and MQTT integration

Root owns experiment_service.*, rtk_base_request.*, rtk_base_experiment.*, main.cpp, CMake and relevant tests. Extend the existing RTK controller and its shared dedup/lease state for five rover verbs. Preserve existing Handle call compatibility using optional operation/sampler callbacks after now; rover ACK fact.rtk contains full service status, old fact.base remains unchanged. ExperimentService::ControlRelay(string_view) implements verbs; Status(now,bool include_samples=true) and Px4Observation/relay stats are additive. Enforce current baseline/identity/frame age, no replay across pause or outage, explicit stopped/paused/waiting states, count send failure and dropped frames, no automatic restart. Keep most recent full RTCM and its full successful MAVLink fragment group as one bounded teaching transfer, plus base/GPS raw samples; high-rate status omits all raw samples and transition history. Test using two PTYs and actual encoded frames. Integrate operation dispatch and registration capabilities without creating a second identity. Extend real process/Broker integration for new operations, pause/resume and rich snapshot vs compact realtime.

### Task 4: Documentation, validation and review

Document exact action schema, all added telemetry fields, bytes/decoded-field mapping, sample windows/age, workflow and server integration needs in one MD. Update README, CLAUDE and current architecture/protocol notices. Run full WSL build + CTest including actual Broker test; independent review. Stage ARM64 build/test and deployment preserving field config only after local checks. Record indoor actual outcomes separately from simulator evidence; no outdoor Fixed claim.
