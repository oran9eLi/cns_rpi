# Device status fault modules implementation plan

> For agentic workers: use superpowers:executing-plans to implement and verify the steps below.

**Goal:** Identify faulty F407 modules in the regular device status frame.

**Architecture:** Add the read-only `fault_modules` field to the existing status payload. Derive it from the same identity-filtered module states and persisted fault progress used by `HealthTracker`; share the module-name table with telemetry serialization.

**Tech stack:** C++23, nlohmann/json, CMake, doctest, Mosquitto, Python.

**Contract:** The user's request adds a field identifying the abnormal module. Implementation choices: a fixed-order string array of the existing 14 module identifiers; immediate OFFLINE/FAILED entries; retained entries for modules whose persisted progress reached 12 hours; `null` when no module state is available and no retained damage cause exists. `[]` means no confirmed faults, not proof that all modules are healthy. Maintenance does not hide faults. Recovery, degradation and deliberate disablement clear the corresponding fault as before.

## Constraints

- Preserve the prior uncommitted `cns_box` correction and test-fixture fix.
- Keep Topic, QoS 1, retain, 10-second cadence, health rules, and persistence format unchanged.
- Preserve normal-exit payload details; document CONNECT-time Last Will snapshots without adding reconnects for module-list changes.
- No Web handshake, server changes, firmware changes, push, release or Pi deployment in this task.

## Review focus

- Unknown or unverified F407 data must not become fourteen invented module faults.
- A persisted damaged state must retain the responsible module names after restart.
- All module names and ordering must agree with existing telemetry.
- Fault changes must publish immediately even when whole-device status stays usable.
- Online, normal offline, and Broker Last Will frames must contain eleven fields.

## Task 1: Implement the field

- [x] Add the eleventh-field assertion to the existing payload test; run it and verify the missing-field failure.
- [x] Add health tests for immediate and multiple faults, excluded states, unknown data, restart, recovery, maintenance and binding changes.
- [x] Share the existing module-name table; expose the derived fault names from `HealthTracker` without changing persisted JSON.
- [x] Pass fault names to all status payload builders in `main.cpp`.
- [x] Test payload values, readonly config rejection and content-change publication.
- [x] Extend the real-process Broker test to check the eleventh field, immediate faults, recovery, persisted damage and offline snapshots.

## Task 2: Verify and document

- [x] Update current contract, examples and desktop handoff; retain historical raw validation records.
- [x] Build and run the full existing CI test driver against a private local Broker.
- [x] Review the diff and document actual validation and deployment limits; leave changes uncommitted unless requested.

## Validation result

- Initial payload test failed because `fault_modules` was absent.
- CMake build passed; private-Broker CI driver passed 72/72 CTest suites and 16/16 dialup checks.
- The real status-process suite passed 138 assertions, including immediate faults, recovery, persisted damage, normal offline and Last Will.
- The RTK gateway strict field set was updated to eleven fields; missing-F407 module details are checked as null.
- Current JSON examples and the diff were verified; independent code review found no blocking issues.
- No commit, push, release or hardware deployment was performed.
- Local test log: `build/fault-modules-20261111/tests.log`.
