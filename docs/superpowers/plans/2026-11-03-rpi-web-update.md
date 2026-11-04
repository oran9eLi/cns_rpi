# Raspberry Pi Web Update Implementation Plan

> Identity and first-deployment details are superseded by `2026-11-03-factory-image.md` and the current server interface contract: F407 device_id, one device key, one poll endpoint; no extra node identity or enrollment API.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pi can install a human-selected CI-built version after the routing server receives a Web-backend maintenance message, and reports the durable result.

**Architecture:** Web backend publishes maintenance/request using the existing MQTT source envelope. The routing server persists a node task and serves release metadata/artifacts; an independent Pi Python/systemd timer polls HTTPS and calls the same staged package installer as first deployment. This repository owns Pi/CI plus an exact server integration contract; the active server worktree remains read-only.

**Tech Stack:** Python 3 standard library, systemd, C++23/CMake, GitHub Actions native ARM64 with Debian trixie containers.

**Spec:** docs/superpowers/specs/2026-11-03-远程维护与持续集成设计.md

## Global Constraints

- trixie ARM64, dcdw runtime user, normal writable root; no target installation in WSL.
- Pi only installs; no Git/compiler requirement. First deployment has LAN/Wi-Fi and apt access.
- Explicit server job only; one app or cellular component per job; no remote maintenance-agent self-update.
- Preserve现场 config, identity, experiment state; app update never restarts cellular.
- Idle poll 60 seconds; app observation 40 seconds; cellular connectivity observation 180 seconds.
- Validate external data once at boundaries; no arbitrary shell commands; keep secrets out of logs/package.
- Server/Web source code is outside this task's write scope; deliver contract and report deployment gaps honestly.

## Review Focus

- Duplicate delivery after lost response must return saved result without reinstalling.
- New tasks must not overwrite an unacknowledged prior result.
- Corrupt/incompatible packages must not stop the running service.
- App failure must restore previous executable and configuration without touching bindings.
- Cellular process active alone must not produce success while server API remains unreachable.

## Shared Interfaces / Package Layout

Archive root: manifest.json; app/bin/{cns_rpi,cns_control_test}; app/scripts/{cns-rpi-apply-config.py,migrate_telemetry_config.py}; app/systemd/cns-rpi.service; cellular/scripts/cellular_dialup.py and cellular_link/; cellular/systemd/cellular-dialup.service; maintenance/*.py; maintenance/systemd/cns-rpi-maintenance.{service,timer}.

Manifest JSON: schema_version=1, version, git_commit (40 hex), platform="trixie-arm64", files (relative path -> sha256), runtime_dependencies (apt package names), build (compiler/cmake metadata). Package excludes tests/secrets.

Links /opt/cns-rpi/current -> releases/<version>-<sha>/app; cellular-current -> .../cellular; maintenance-current -> .../maintenance. Metadata is the parent manifest. Stable helper links to current/scripts/cns-rpi-apply-config.py.

Protocol module scripts/maintenance/protocol.py produces immutable Artifact(version:str, git_commit:str, platform:str, url:str, sha256:str) and Job(job_id:str, action:str, component:str, artifact:Artifact|None). parse_poll(raw:dict, allowed_origin:str) -> tuple[str|None,Job|None]; parse_artifact(raw:dict, allowed_origin:str) -> Artifact. Downloads stay on configured HTTPS server origin (loopback HTTP allowed for local development tests).

Installer module scripts/maintenance/install_release.py exports Installer(root:Path=Path("/"), services=None). stage(archive:Path, artifact:Artifact) -> Path returns verified release directory. bootstrap(release:Path, business_config:Path, maintenance_config:Path) -> dict; apply(release:Path, component:str, network_probe:Callable[[],bool]) -> dict; rollback(component:str, network_probe:Callable[[],bool]) -> dict; status() -> dict. Results have status in succeeded/failed/rolled_back/needs_attention, component, current_version, previous_version, error. Runtime components read parent manifest. Systemd is the external boundary and may be replaced in tests; tests run real staging/link/config/install logic.

Client scripts/maintenance/maintenance_client.py --config path executes one poll, reports platform/components/services and job (current phase or terminal result); response schema_version=1, acknowledged_job_id=null|id, job=null|object. Artifact wire fields: version, git_commit, platform, artifact_url, sha256. Job install adds job_id,action,component; rollback has no artifact. Server acks terminal result only after persisting it. Client saves accepted job before mutation and terminal result before reporting, retains until matching ack; duplicate jobs never reinstall; interrupted nonterminal jobs become needs_attention.

Bootstrap scripts/bootstrap_pi.py standalone: --version V --maintenance-config file --business-config file. Config: schema_version=1, server_url, node_id, token, optional ca_file. GET /api/maintenance/v1/releases/{version}?platform=trixie-arm64 returns artifact fields. Root/platform/writable-root checks before apt. Install runtime deps then execute verified package installer. Do not rewrite apt mirrors.

### Task 1: Package installer and first-deployment entry

**Files:** scripts/maintenance/install_release.py, scripts/bootstrap_pi.py, systemd/release/*, tests/test_maintenance_install.py, tests/test_bootstrap_pi.py.
**Consumes:** Artifact and package layout above. **Produces:** Installer API above and standalone bootstrap.

- [ ] Write tests that execute real archive staging/config/link transitions in a temporary root with a systemd boundary double. Cover initial/preserved config, bad checksum/path/platform, no 5G restart for app, app failure rollback, manual rollback compatibility, cellular API failure rollback.
- [ ] Run python3 -m unittest tests.test_maintenance_install tests.test_bootstrap_pi -v; expect feature-missing failure first.
- [ ] Implement installer and bootstrap, reusing existing config migrator/validators. New release units have absolute /opt paths and unchanged runtime/user/watchdog semantics.
- [ ] Run the tests above; expect all pass. Bootstrap platform/dependency tests must not run real apt/systemctl.
- [ ] Commit the owned files only.

### Task 2: CI build, broker regression, package generation

**Files:** scripts/build_release_package.py, scripts/ci/*, .github/workflows/ci.yml, .github/workflows/release.yml, tests/test_release_package.py.
**Consumes:** package layout, Task 1 units/bootstrap, maintenance scripts. **Produces:** cns-rpi-V-trixie-arm64.tar.gz, .sha256, manifest.json, bootstrap_pi.py.

- [ ] Write a real packager test with controlled ELF fixtures; assert generated tar digests, exclusion of config/secrets/tests, exact version/platform mismatch refusal and executable modes.
- [ ] Run python3 -m unittest tests.test_release_package -v; expect missing packager failure.
- [ ] Implement a strict ARM64 packager and runtime dependency manifest. Release tag matches CMake project version; native ARM64 trixie container uses distro GCC/CMake, no march=native.
- [ ] Add CI test driver that owns an isolated loopback Mosquitto and actively stops/restarts it when existing test marker lines appear. Set both CNS_TEST_MQTT_* env vars; run CTest and scripts/test_cellular_dialup.py.
- [ ] Add PR/push CI and v* tag release workflow, runtime-only container smoke test, artifacts/test logs, release publishing only after success.
- [ ] Run packager/driver tests and local available syntax checks. Record ARM64/GitHub checks unavailable locally explicitly.
- [ ] Commit owned files only.

### Task 3: Durable Pi task client and Web/server contract

**Files:** scripts/maintenance/{protocol.py,maintenance_client.py}, config/maintenance.example.json, tests/test_maintenance_client.py, docs/远程更新接口契约.md, docs/远程更新部署手册.md, CMakeLists.txt, README.md.
**Consumes:** Installer API, server release/poll endpoints. **Produces:** independent authenticated poll/install/report flow and concrete MQTT-to-job contract.

- [ ] Write HTTP integration tests using a real local HTTP server and real worker/state store. Assert idle no mutation, install success and report, redelivery no reinstall, acknowledgement/new job ordering, interrupted task handling, invalid/off-origin input rejection and no token leakage.
- [ ] Run python3 -m unittest tests.test_maintenance_client -v; expect missing feature failure.
- [ ] Implement protocol boundary types and client. Client downloads with stdlib urllib using configured CA; no cross-origin credentials/redirects. Installer boundary may be replaced for client tests; add one client+real-installer integration test.
- [ ] Document exact Web MQTT request/ack, server node mapping/persistence/release import/poll responsibilities, separate long-lived job status from existing config/control command timeout.
- [ ] Document concrete bootstrap, initial node config, version import, install/rollback procedure; keep real endpoints/tokens as placeholders.
- [ ] Register new Python suites in CTest, configure/build, run full CTest plus standalone cellular test and actual local MQTT checks.
- [ ] Fresh branch review; fix important findings with reproducing tests and run relevant/full suites. Commit; report code/build/CI/physical Pi/server integration evidence separately.
