# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

V1 is implemented and deployed. Milestones M1–M6 plus the runtime config-command link are done; the remaining V1 gaps are product hardening (OverlayFS read-only root, physical power-cut acceptance), production networking (real SIM/APN, MQTT TLS + per-device credentials/ACL), and end-to-end integration with the server-side `route_service`, which is not yet live. See `docs/V1设计文档.md` §10 for per-milestone status and `docs/M7系统化部署设计.md` for the deployment/hardening checklist.

### Build and test

```bash
cmake -S . -B build && cmake --build build   # -Wall -Wextra enforced
ctest --test-dir build --output-on-failure
```

Dependencies: `nlohmann_json` (>=3.2.0), `libmosquitto` (via pkg-config), `doctest`, `python3`. On a fresh Pi run `scripts/install_deps.sh` first — it switches apt to the TUNA mirror and installs the build dependencies.

Build verification follows `docs/协作规则.md` §6: every change must build clean under `-Wall -Wextra` and keep `ctest` fully green.

### Deployment

`scripts/deploy.sh` is the only supported deployment path. It is idempotent and refuses to run outside `/home/dcdw/cns_rpi` as user `dcdw`. It builds, then installs the config helper, `cns-rpi.service`, `cellular-dialup.service` and the journald drop-in, and restarts the main service.

`config/config.json` is gitignored — it is per-device field configuration. `config/config.example.json` is the tracked template; keep the two in sync in shape, never commit real field values.

## What this repository is

RPi-side (Raspberry Pi 5, trixie/ARM64, C++23) data node for a CNS (Communication/Navigation/Surveillance) vocational-training kit. It is one of three independently-maintained parts of the product:

- **STM32F407 + FreeRTOS firmware** — separate repo (`oran9eLi/Formal_Framework`, branch `fj-lora`), maintained by the firmware architect. Not present in this working tree; treat its MAVLink message definitions and UART allocations as an external dependency, not something to modify here.
- **This repo** — the RPi node.
- **硬件部/软件部 servers** — downstream, out of scope.

Read `docs/V1设计文档.md` before making architectural changes; it is the source of truth for scope and is required to stay in sync with the code per `docs/协作规则.md` §7.

## Architecture (from design docs — read these for full detail)

**F407 data flow is bidirectional over its own UART link carrying MAVLink v2, common dialect only (no custom dialect XML).** In the field the link is a CH340 USB-serial adapter whose device node is not stable across reboots, so `serial.device` may remain `auto`; do not hardcode a `/dev/ttyUSBn` path. Optional RTK adds two separate explicit paths for PX4/3DR and the base USB receiver. F407 discovery excludes both RTK devices, including aliases resolving to the same device.

- Upstream (STM32→RPi): HEARTBEAT, standard telemetry messages, `NAMED_VALUE_INT`/`TUNNEL`-encoded extension frames, `OPEN_DRONE_ID_*` identity frames → decoded → published as JSON over MQTT (RPi is a plain MQTT client; it does **not** run a local Mosquitto broker).
- Downstream (management center → MQTT → RPi → STM32): commands arrive as JSON, get dispatched, encoded as `COMMAND_LONG`, sent over UART; `COMMAND_ACK` results get reported back over MQTT. STM32 is the authority for command validation — RPi only does JSON schema sanity-checking before encoding.

**Core extensibility principle (V1设计文档.md §3)**: the MAVLink decode layer and the command-forwarding layer are each decoupled from *who consumes/produces* the data, via two internal seams:
- `state/` — decoded STM32 state, written by `protocol/` decoders, read by consumers (V1: only the MQTT publisher; V2 will add a Qt renderer and a camera/OpenCV pipeline as additional readers without touching the decoder).
- `command/` — a `command_source` interface + internal dispatcher; V1 has one source (MQTT), later sources (e.g. a local Qt UI in V2) plug in without touching the STM32-facing encode/dispatch logic.

Vendor the official `mavlink/c_library_v2` headers (`common`/`standard`/`minimal`) from the firmware repo's `Third_Party/mavlink/` verbatim — same header files on both ends, don't hand-roll frame parsing.

## Device identity (docs/设备标识符.md — do not shortcut this by reading only V1设计文档.md §6)

Multiple identifiers exist for different owners; don't conflate them:

- **`device_id` — the single CNS box business key, passively received from F407's `OPEN_DRONE_ID_BASIC_ID.uas_id`** (`docs/设备标识符.md`). It addresses MQTT topics, the MQTT Client ID, registration, telemetry and command routing, including optional RTK experiment targets. PX4 Basic ID is experiment metadata only. There is no second gateway/base business identifier: `vendor_id`, `remote_id`, `gateway_id` and `rpi_serial` were all deleted from state and from the wire protocol — do not reintroduce them.
- **The RPi owns no business identity.** It is a MAVLink↔MQTT gateway; swapping or reimaging it must not change any `device_id`. `/proc/cpuinfo` serial is no longer read.
- `DCDW-XXX` — school-facing label, derived from firmware's `PX4LITE_UNIT_ID`. Unique only *within one school*, routinely repeats across schools. Never use as a global key.
- For the CNS box, that `uas_id` *is* the vendor unique product ID (`DCDWCNS1` + 12-char SN), structured per GB/T 41300 (MFC+PMC+SN, 20 chars, charset `0-9A-Z` minus `O`/`I`). SN is a SHA-256-truncated hash of the STM32 chip's 96-bit UID. `DCDW`/`CNS1` are product metadata parsed out of the prefix — not identity, not a firmware version.
- **SN is computed on STM32, not RPi** — because RemoteID broadcast (`STM32 → UART4 → ESP32-S3`, does not pass through RPi) needs the value locally before it ever talks to RPi. RPi receives the already-computed SN and reuses it verbatim; it must not recompute it.
- RemoteID's GB 46750 "唯一产品识别码" field reuses this same vendor ID/SN — one hash, not two.
- **The default endpoint is the F407 CNS box**, with `MAV_TYPE_ONBOARD_CONTROLLER` and `MAV_AUTOPILOT_INVALID`. Its persistent binding may restore the control channel while live identity is verified. Product metadata comes from the F407 identity prefix; registration carries no inferred hardware/firmware version. Firmware-native `Px4Lite` / `px4lite_*` names are unrelated to gateway device modes and must remain intact.
- **Optional `rtk_base.enabled=true` adds RTK to the same CNS box main loop.** The Pi normally stays connected to F407 through `serial`; `rtk_base.px4_device/px4_baud` own the separate 3DR/PX4 link and `rtk_base.device/baud` own the base USB receiver. All three devices must differ. Keep the existing F407 binding, `cns_box` registration, runtime state, MQTT connection, config/control and G2 flows. Add `telemetry.rtk={base,px4,relay,observation,box_identity_verified}`. With a valid persisted box binding and no identity conflict, RTK may publish under the cached box identity even while F407 is disconnected. When F407 business is not live, those frames contain RTK facts without stale F407 ordinary sensor data; runtime remains truthfully cached/offline/unknown. Local status includes `box:{device_id,identity_verified}`. Without a box binding, observe base/PX4 locally only. PX4 heartbeat/Basic ID establish only the subordinate experiment session, never MQTT identity; PX4 loss clears its live data without closing the box MQTT session. RTK actions use the box experiment topic/target and shared ACK outbox. Base configuration is RAM-only, with stop-epoch confirmation and exact readback; RTCM forwarding is explicit via rtk_rover_start/pause/resume/stop/status; never auto-start it. Use only CRC-valid fresh data after base completion and live PX4 verification. Low-frequency snapshots carry bounded actual UBX/RTCM/GPS and successful MAVLink wire samples; realtime omits raw/history. Fresh GPS epochs, not heartbeats or duplicate timestamps, establish current fix. Serial write is not receiver ACK. See docs/RTK移动站实验与教学数据接口.md. The earlier independent PX4 registration tests are historical component evidence and do not validate this three-link box topology. See `docs/RTK基站第一实验操作与接口.md`.
- Cached RTK schema 3 snapshots keep exactly the empty root container `"drone_id":{"basic_id":{}}` because the existing server `ParseTelemetry` requires both objects. Do not populate `id_type`, `ua_type` or F407 live observations; schema 1 realtime still omits `drone_id`. This is a Pi compatibility change; server code is unchanged.

## Collaboration conventions (docs/协作规则.md)

- Commit messages: `<type>: <简短中文说明>` (types: feat/fix/docs/refactor/chore/build/test), Chinese description, no long bodies.
- Source comments: Doxygen-style, Chinese, on file headers / public functions / structs — explain *why/who calls/what's off-limits*, not a translation of the code.
- Default branch `main`; feature branches `<type>/<short-desc>`.
- Architecture/protocol-scope/identity changes must be reflected in `docs/V1设计文档.md` (or the doc it points to) in the same change — do not let code and docs drift.
