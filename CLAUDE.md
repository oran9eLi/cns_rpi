# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

This repository is currently in the design phase — no source code exists yet. `docs/` contains the full V1 design; `CMakeLists.txt` / `src/` will appear starting at milestone M1 (see `docs/V1设计文档.md` §10). There are no build/lint/test commands to run yet. Once the CMake skeleton lands, build verification follows `docs/协作规则.md` §6 (CMake build with `-Wall -Wextra`, unit tests under `tests/` for UART/MAVLink parsing and MQTT edge cases).

## What this repository is

RPi-side (Raspberry Pi 5, trixie/ARM64, C++23) data node for a CNS (Communication/Navigation/Surveillance) vocational-training kit. It is one of three independently-maintained parts of the product:

- **STM32F407 + FreeRTOS firmware** — separate repo (`oran9eLi/Formal_Framework`, branch `fj-lora`), maintained by the firmware architect. Not present in this working tree; treat its MAVLink message definitions and UART allocations as an external dependency, not something to modify here.
- **This repo** — the RPi node.
- **硬件部/软件部 servers** — downstream, out of scope.

Read `docs/V1设计文档.md` before making architectural changes; it is the source of truth for scope and is required to stay in sync with the code per `docs/协作规则.md` §7.

## Architecture (from design docs — read these for full detail)

**Data flow is bidirectional over a single new UART link (not yet allocated by firmware) carrying MAVLink v2, common dialect only (no custom dialect XML):**

- Upstream (STM32→RPi): HEARTBEAT, standard telemetry messages, `NAMED_VALUE_INT`/`TUNNEL`-encoded extension frames, `OPEN_DRONE_ID_*` identity frames → decoded → published as JSON over MQTT (RPi is a plain MQTT client; it does **not** run a local Mosquitto broker).
- Downstream (management center → MQTT → RPi → STM32): commands arrive as JSON, get dispatched, encoded as `COMMAND_LONG`, sent over UART; `COMMAND_ACK` results get reported back over MQTT. STM32 is the authority for command validation — RPi only does JSON schema sanity-checking before encoding.

**Core extensibility principle (V1设计文档.md §3)**: the MAVLink decode layer and the command-forwarding layer are each decoupled from *who consumes/produces* the data, via two internal seams:
- `state/` — decoded STM32 state, written by `protocol/` decoders, read by consumers (V1: only the MQTT publisher; V2 will add a Qt renderer and a camera/OpenCV pipeline as additional readers without touching the decoder).
- `command/` — a `command_source` interface + internal dispatcher; V1 has one source (MQTT), later sources (e.g. a local Qt UI in V2) plug in without touching the STM32-facing encode/dispatch logic.

Vendor the official `mavlink/c_library_v2` headers (`common`/`standard`/`minimal`) from the firmware repo's `Third_Party/mavlink/` verbatim — same header files on both ends, don't hand-roll frame parsing.

## Device identity (docs/设备标识符.md — do not shortcut this by reading only V1设计文档.md §6)

Multiple identifiers exist for different owners; don't conflate them:

- `DCDW-XXX` — school-facing label, derived from firmware's `PX4LITE_UNIT_ID`. Unique only *within one school*, routinely repeats across schools. Never use as a global key.
- Vendor unique product ID (`DCDWCNS1` + 12-char SN) — the authoritative global device key, structured per GB/T 41300 (MFC+PMC+SN, 20 chars, charset `0-9A-Z` minus `O`/`I`). SN is a SHA-256-truncated hash of the STM32 chip's 96-bit UID.
- **SN is computed on STM32, not RPi** — because RemoteID broadcast (`STM32 → UART4 → ESP32-S3`, does not pass through RPi) needs the value locally before it ever talks to RPi. RPi receives the already-computed SN and reuses it verbatim; it must not recompute it.
- RemoteID's GB 46750 "唯一产品识别码" field reuses this same vendor ID/SN — one hash, not two.
- RPi's own hardware serial (`/proc/cpuinfo`) is a V1-only stopgap authoritative key, used until the vendor-ID pipeline above is fully wired end to end.

## Collaboration conventions (docs/协作规则.md)

- Commit messages: `<type>: <简短中文说明>` (types: feat/fix/docs/refactor/chore/build/test), Chinese description, no long bodies.
- Source comments: Doxygen-style, Chinese, on file headers / public functions / structs — explain *why/who calls/what's off-limits*, not a translation of the code.
- Default branch `main`; feature branches `<type>/<short-desc>`.
- Architecture/protocol-scope/identity changes must be reflected in `docs/V1设计文档.md` (or the doc it points to) in the same change — do not let code and docs drift.
