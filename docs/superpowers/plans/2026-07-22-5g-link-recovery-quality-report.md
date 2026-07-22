# 5G 链路自恢复与质量上报实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将现有 RM500U 开机拨号脚本升级为常驻链路守护服务，在断链时分级自恢复，并把同一份 5G 状态快照用于 MQTT 质量上报和原有 STM32 `RPICELL` 在线状态。

**Architecture:** `cellular-dialup.service` 是唯一 AT 口所有者，常驻执行首次拨号、绑定 `usb0` 的双目标探测、质量采集和恢复状态机，并原子写入 `/run/cns-rpi/cellular_status.json`。C++ 主程序只读取快照：公开字段进入 `telemetry.cellular_5g`，内部诊断字段继续生成原有 `RPICELL` 位图。

**Tech Stack:** Python 3 标准库、pyserial、systemd、Linux sysfs/ip/ping、C++23、nlohmann/json、doctest、CMake/CTest。

## Global Constraints

- 所有文档、回答、代码注释和运行日志使用中文；Git 提交前缀使用英文，说明使用中文。
- 新增或修改的日志、注释和文档采用专业、客观的表述，避免口语化措辞。
- 设计依据：`docs/superpowers/specs/2026-07-22-5g-link-recovery-quality-report-design.md`。
- RM500U 型号为 `RM500U-CNV`，实测固件为 `RM500UCNVAAR03A14M2G`。
- 质量采集以 `AT+QENG="servingcell"` 和 `AT+CSQ` 为准，不使用实机返回失败的 `AT+QCSQ`。
- 公网探测目标固定从配置读取，默认 `112.124.52.232`、`119.29.29.29`，并强制绑定 `usb0`。
- 只有两个目标连续 3 轮均失败才恢复；连续 2 轮均成功才恢复为 `ONLINE`。
- 三次重拨失败后允许执行 `AT+CFUN=0,1`；退避从 15 秒开始，最大 300 秒。
- STM32 接口保持 MAVLink `NAMED_VALUE_INT("RPICELL")`，不增加固件字段；`ONLINE`、`DEGRADED` 均置在线位。
- 守护服务与主程序统一读取 `/var/lib/cns-rpi/config.json`。
- 快照内部 `diagnostics` 不进入 MQTT；公开字段位于 `payload.telemetry.cellular_5g`。
- 现有工作区 `scripts/cellular_dialup.py` 的中文日志措辞改动属于已有工作，实施时保留并纳入对应代码提交，禁止丢弃。
- 每确认一项新的实现选择或实机事实，必须在同一任务中同步相关文档。

---

## 文件结构

- Create: `scripts/cellular_link.py` — 纯领域逻辑：配置、AT 响应解析、状态机、退避和快照模型。
- Modify: `scripts/cellular_dialup.py` — Linux/串口适配器和常驻守护循环；继续作为 systemd 入口。
- Create: `tests/test_cellular_link.py` — 纯领域逻辑单元测试。
- Create: `tests/test_cellular_dialup.py` — 拨号、探测、快照和恢复编排测试。
- Create: `src/cellular/cellular_snapshot.hpp` — C++ 快照类型、读取接口和链路状态枚举。
- Create: `src/cellular/cellular_snapshot.cpp` — JSON 读取、mtime 过期判断和公开遥测 JSON 构造。
- Modify: `src/cellular/cellular_status.hpp/.cpp` — 从快照映射既有 `RPICELL` 位图，不再自行探测网卡。
- Create: `tests/test_cellular_snapshot.cpp` — 快照读取、过期、损坏及公开字段测试。
- Modify: `tests/test_cellular_status.cpp` — 五种状态到原有位图的兼容测试。
- Modify: `src/payload/json_serializer.hpp/.cpp` — 注入 `telemetry.cellular_5g`。
- Modify: `tests/test_json_serializer.cpp` — 遥测字段与既有字段回归测试。
- Modify: `src/main.cpp` — 每次使用前读取同一快照，分别供 `RPICELL` 和遥测发布使用。
- Modify: `src/config/app_config.hpp/.cpp`、`tests/test_app_config.cpp` — C++ 快照路径和过期阈值配置。
- Modify: `config/config.example.json` — 补齐守护服务和 C++ 消费端配置。
- Modify: `systemd/cellular-dialup.service` — oneshot 改为常驻服务并统一配置路径。
- Modify: `tests/test_cns_rpi_service.cpp` — systemd 配置契约测试。
- Modify: `CMakeLists.txt` — 注册 Python 和 C++ 新测试。
- Modify: `scripts/deploy.sh` — 部署后重启常驻拨号服务，避免旧进程继续运行旧代码。
- Modify: `docs/technical_docs/01_5G模块技术文档.md`、`docs/V1设计文档.md`、`docs/M7系统化部署设计.md` — 同步最终实现和验收事实。

---

### Task 1: 纯领域模型、AT 解析和状态机

**Files:**
- Create: `scripts/cellular_link.py`
- Create: `tests/test_cellular_link.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `CellularConfig.from_json(root: dict) -> CellularConfig`
- Produces: `parse_qeng(lines: list[str]) -> RadioMetrics`
- Produces: `parse_csq(lines: list[str]) -> Optional[int]`
- Produces: `parse_cops(lines: list[str]) -> Optional[str]`
- Produces: `LinkStateMachine.observe(basic_ready: bool, target_results: list[bool]) -> LinkState`
- Produces: `RecoveryBackoff.next_delay() -> int`、`reset() -> None`
- Produces: `CellularSnapshot.to_dict(include_diagnostics: bool = True) -> dict`

- [ ] **Step 1: 写配置与 AT 解析失败测试**

在 `tests/test_cellular_link.py` 使用 `unittest` 写出以下基线：

```python
class ParseTest(unittest.TestCase):
    def test_defaults_and_invalid_ranges(self):
        cfg = CellularConfig.from_json({"apn": "x", "cid": 1,
            "usb_interface_number": "05", "at_port_wait_seconds": 30,
            "interface_name": "usb0", "heartbeat_interval_ms": 1000})
        self.assertEqual(cfg.probe_targets, ("112.124.52.232", "119.29.29.29"))
        self.assertEqual(cfg.probe_interval_seconds, 10)
        with self.assertRaisesRegex(ValueError, "probe_targets不能为空"):
            CellularConfig.from_json({**dataclasses.asdict(cfg), "probe_targets": []})

    def test_parse_real_nr5g_sa_response(self):
        metrics = parse_qeng(['+QENG: "servingcell","CONNECT","NR5G-SA","FDD",460,00,'
                              'A08EF0011,463,100039,152650,28,30,-87,-10,-3,20,0,0'])
        self.assertEqual((metrics.access_technology, metrics.rsrp_dbm,
                          metrics.rsrq_db, metrics.sinr_db),
                         ("NR5G-SA", -87, -10, -3))

    def test_parse_csq_and_unknown(self):
        self.assertEqual(parse_csq(["+CSQ: 19,99"]), -75)
        self.assertIsNone(parse_csq(["+CSQ: 99,99"]))
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python3 -m unittest -v tests.test_cellular_link`

Expected: FAIL，错误包含 `ModuleNotFoundError: No module named 'scripts.cellular_link'`。

- [ ] **Step 3: 实现配置、枚举、快照模型和三类 AT 解析器**

在 `scripts/cellular_link.py` 定义不可变数据类型，并针对 `QENG` 的
`NR5G-SA`、`NR5G-NSA`、`LTE` 分支按移远字段顺序解析。实现至少包括：

```python
class LinkState(enum.Enum):
    UNKNOWN = "UNKNOWN"
    ONLINE = "ONLINE"
    DEGRADED = "DEGRADED"
    OFFLINE = "OFFLINE"
    RECOVERING = "RECOVERING"

@dataclasses.dataclass(frozen=True)
class RadioMetrics:
    access_technology: Optional[str] = None
    rsrp_dbm: Optional[int] = None
    rsrq_db: Optional[int] = None
    sinr_db: Optional[int] = None

@dataclasses.dataclass(frozen=True)
class CellularConfig:
    apn: str
    cid: int
    usb_interface_number: str
    at_port_wait_seconds: int
    interface_name: str = "usb0"
    probe_targets: tuple[str, ...] = ("112.124.52.232", "119.29.29.29")
    probe_interval_seconds: int = 10
    offline_failure_threshold: int = 3
    online_success_threshold: int = 2
    signal_sample_interval_seconds: int = 30
    redial_attempts_before_reset: int = 3
    recovery_delay_seconds: int = 15
    recovery_delay_max_seconds: int = 300

def parse_csq(lines):
    match = next((re.fullmatch(r"\+CSQ:\s*(\d+),(\d+)", line.strip())
                  for line in lines if line.strip().startswith("+CSQ:")), None)
    if match is None or int(match.group(1)) == 99:
        return None
    return -113 + 2 * int(match.group(1))
```

`CellularSnapshot` 必须包含设计规格第 6 节全部公开字段以及四个内部诊断布尔值；
`last_recover_at` 使用 `Optional[str]`，不可用值使用 `None`。

- [ ] **Step 4: 写状态迟滞和退避失败测试**

```python
class StateMachineTest(unittest.TestCase):
    def test_three_double_failures_offline_and_two_successes_online(self):
        machine = LinkStateMachine(offline_threshold=3, online_threshold=2)
        self.assertEqual(machine.observe(True, [False, False]), LinkState.DEGRADED)
        self.assertEqual(machine.observe(True, [False, False]), LinkState.DEGRADED)
        self.assertEqual(machine.observe(True, [False, False]), LinkState.OFFLINE)
        machine.mark_recovering()
        self.assertEqual(machine.state, LinkState.RECOVERING)
        self.assertEqual(machine.observe(True, [True, True]), LinkState.RECOVERING)
        self.assertEqual(machine.observe(True, [True, True]), LinkState.ONLINE)

    def test_one_target_reachable_is_degraded_without_offline_count(self):
        machine = LinkStateMachine(3, 2)
        for _ in range(5):
            self.assertEqual(machine.observe(True, [True, False]), LinkState.DEGRADED)

    def test_backoff_caps_at_maximum(self):
        backoff = RecoveryBackoff(15, 300)
        self.assertEqual([backoff.next_delay() for _ in range(7)],
                         [15, 30, 60, 120, 240, 300, 300])
```

- [ ] **Step 5: 实现状态机和退避并跑通测试**

`basic_ready=False` 立即得到 `OFFLINE`；一个目标成功得到 `DEGRADED` 并清除
双失败计数；只有全成功才累计上线计数。实现后运行：

Run: `python3 -m unittest -v tests.test_cellular_link`

Expected: PASS，覆盖配置、SA/NSA/LTE/异常解析、迟滞和退避。

- [ ] **Step 6: 注册 CTest 并提交**

在 `CMakeLists.txt` 增加：

```cmake
add_test(NAME cellular_link_python
    COMMAND ${PYTHON3_EXECUTABLE} -m unittest -v tests.test_cellular_link)
set_tests_properties(cellular_link_python PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```

Run: `cmake -S . -B build && ctest --test-dir build -R cellular_link_python --output-on-failure`

Expected: `100% tests passed`。

Commit:

```bash
git add scripts/cellular_link.py tests/test_cellular_link.py CMakeLists.txt
git commit -m "feat: 增加5G链路状态机与质量解析"
```

---

### Task 2: Linux 探测、质量采集和原子快照

**Files:**
- Modify: `scripts/cellular_dialup.py`
- Create: `tests/test_cellular_dialup.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 的 `CellularConfig`、`RadioMetrics`、`CellularSnapshot` 和解析器。
- Produces: `probe_interface(interface_name: str) -> InterfaceStatus`
- Produces: `probe_target(interface_name: str, target: str, timeout_seconds: int = 2) -> bool`
- Produces: `collect_radio_metrics(ser) -> RadioMetrics`
- Produces: `write_snapshot_atomic(path: pathlib.Path, snapshot: CellularSnapshot) -> None`

- [ ] **Step 1: 写强制网卡探测和快照失败测试**

```python
class RuntimeAdapterTest(unittest.TestCase):
    @mock.patch("scripts.cellular_dialup.subprocess.run")
    def test_probe_target_binds_interface(self, run):
        run.return_value = subprocess.CompletedProcess([], 0, "", "")
        self.assertTrue(probe_target("usb0", "112.124.52.232"))
        self.assertEqual(run.call_args.args[0],
                         ["ping", "-I", "usb0", "-c", "1", "-W", "2",
                          "112.124.52.232"])

    def test_write_snapshot_replaces_complete_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "cellular_status.json"
            write_snapshot_atomic(path, sample_snapshot())
            data = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(data["link_state"], "ONLINE")
            self.assertTrue(data["diagnostics"]["carrier_up"])
            self.assertEqual(list(path.parent.glob("*.tmp")), [])
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python3 -m unittest -v tests.test_cellular_dialup.RuntimeAdapterTest`

Expected: FAIL，提示 `probe_target` 或 `write_snapshot_atomic` 未定义。

- [ ] **Step 3: 实现 Linux 适配器**

保留现有 `find_at_port`、`send_at_command`、PDP/NCM 拨号逻辑和工作区中的中文
日志措辞。新增实现必须满足：

```python
def probe_target(interface_name, target, timeout_seconds=2):
    result = subprocess.run(
        ["ping", "-I", interface_name, "-c", "1", "-W", str(timeout_seconds), target],
        capture_output=True, text=True, timeout=timeout_seconds + 2, check=False)
    return result.returncode == 0

def write_snapshot_atomic(path, snapshot):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(snapshot.to_dict(), ensure_ascii=False,
                                    separators=(",", ":")) + "\n", encoding="utf-8")
    os.replace(temporary, path)
```

实际实现要给同目录临时文件加入 PID，且在异常路径清理临时文件。
`probe_interface` 读取 sysfs、`ip -j address show dev` 和 `ip route show default dev`，
返回接口、载波、地址、默认路由、首个非链路本地 IP 和流量计数。

- [ ] **Step 4: 写 AT 质量采集的部分失败测试**

模拟 `QENG` 成功、`CSQ` 成功、`COPS` 失败，断言仍返回制式和三个无线指标，
运营商为 `None`，错误文本包含 `COPS`；再覆盖 AT 口消失和 sysfs 计数缺失。

- [ ] **Step 5: 实现质量采集并跑通 Python 测试**

质量采集按 `COPS?`、`QENG="servingcell"`、`CSQ` 顺序串行查询；单条失败不得
丢弃其他结果。Run:

`python3 -m unittest -v tests.test_cellular_link tests.test_cellular_dialup`

Expected: PASS。

- [ ] **Step 6: 注册 CTest 并提交**

```cmake
add_test(NAME cellular_dialup_python
    COMMAND ${PYTHON3_EXECUTABLE} -m unittest -v tests.test_cellular_dialup)
set_tests_properties(cellular_dialup_python PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```

Commit:

```bash
git add scripts/cellular_dialup.py tests/test_cellular_dialup.py CMakeLists.txt
git commit -m "feat: 增加5G链路探测与状态快照"
```

---

### Task 3: 常驻拨号与分级自恢复

**Files:**
- Modify: `scripts/cellular_dialup.py`
- Modify: `tests/test_cellular_dialup.py`
- Modify: `config/config.example.json`
- Modify: `systemd/cellular-dialup.service`

**Interfaces:**
- Consumes: Tasks 1–2 的状态机、适配器和快照写入器。
- Produces: `CellularDaemon.run() -> None`、`run_once(now_monotonic: float) -> float`。

- [ ] **Step 1: 写首次拨号和正常监测失败测试**

使用假时钟、假串口工厂和注入的适配器，断言：启动发现 AT 口后拨号一次；正常
在线时不重复拨号；每 10 秒探测并写快照；每 30 秒才查询一次质量。

```python
daemon = CellularDaemon(cfg, adapters=fakes, snapshot_path=path)
self.assertEqual(daemon.run_once(0), 10)
self.assertEqual(fakes.dial_count, 1)
self.assertEqual(fakes.quality_count, 1)
self.assertEqual(daemon.run_once(10), 20)
self.assertEqual(fakes.dial_count, 1)
```

- [ ] **Step 2: 写三次重拨后软重启和退避失败测试**

构造持续双目标失败，断言第 3 轮进入一次恢复事件并将 `recover_count` 加 1；三次
重拨失败后只发送一次 `AT+CFUN=0,1`；随后关闭旧串口、重新发现动态 AT 口；仍
失败时下一运行时刻依次增加 15、30、60 秒。同一恢复事件内的重拨和软重启不
重复增加 `recover_count`。再用已有 `/run` 快照启动一个新 daemon，断言从旧值
接续计数；用 2024 年和 2026 年的假墙上时钟分别断言 `last_recover_at` 为 `None`
和带时区的 ISO 8601 字符串。

- [ ] **Step 3: 实现可测试的守护循环**

`CellularDaemon` 构造函数注入单调时钟、墙上时钟、sleep、AT 口查找、串口工厂、
网卡探测和公网探测函数。生产 `main()` 只负责加载 JSON、构造真实适配器并调用：

```python
def main():
    if len(sys.argv) != 2:
        print(f"用法: {sys.argv[0]} <config.json路径>", file=sys.stderr)
        return 2
    config = load_cellular_config(sys.argv[1])
    CellularDaemon(config, snapshot_path=pathlib.Path(
        "/run/cns-rpi/cellular_status.json")).run()
    return 0
```

捕获 `SIGTERM`/`SIGINT` 后结束循环、关闭串口并写最后快照；普通断链和 AT 异常
留在循环内恢复，不退出进程。只有配置错误或不可恢复的程序错误返回非零。

- [ ] **Step 4: 跑通恢复编排测试**

Run: `python3 -m unittest -v tests.test_cellular_dialup`

Expected: PASS，且测试不访问真实串口、网卡或公网。

- [ ] **Step 5: 更新配置示例和 systemd 单元**

在 `config/config.example.json` 的 `cellular` 节加入设计默认值。把单元改为：

```ini
[Service]
Type=simple
User=dcdw
RuntimeDirectory=cns-rpi
RuntimeDirectoryMode=0700
ExecStart=/usr/bin/python3 /home/dcdw/cns_rpi/scripts/cellular_dialup.py /var/lib/cns-rpi/config.json
Restart=on-failure
RestartSec=15
```

删除 `RemainAfterExit=yes`。与 `cns-rpi.service` 共享 RuntimeDirectory，保证任一
服务运行时目录都存在且 `dcdw` 可读写。

- [ ] **Step 6: 提交常驻守护服务**

```bash
git add scripts/cellular_dialup.py tests/test_cellular_dialup.py \
  config/config.example.json systemd/cellular-dialup.service
git commit -m "feat: 实现5G链路常驻监测与自恢复"
```

---

### Task 4: C++ 快照读取与 RPICELL 兼容

**Files:**
- Create: `src/cellular/cellular_snapshot.hpp`
- Create: `src/cellular/cellular_snapshot.cpp`
- Create: `tests/test_cellular_snapshot.cpp`
- Modify: `src/cellular/cellular_status.hpp`
- Modify: `src/cellular/cellular_status.cpp`
- Modify: `tests/test_cellular_status.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `cellular::StatusSnapshot`、`cellular::LinkState`
- Produces: `ReadStatusSnapshot(path, max_age, now) -> StatusSnapshot`
- Produces: `BuildPublicTelemetryJson(const StatusSnapshot&) -> nlohmann::json`
- Produces: `LinkStatus FromSnapshot(const StatusSnapshot&)`

- [ ] **Step 1: 写快照读取失败测试**

覆盖完整 JSON、可空字段、缺少文件、损坏 JSON 和 mtime 超过 30 秒。损坏、缺失、
过期都返回 `UNKNOWN`，不抛异常；过期保留最后观测值并把 `last_error` 改为
`5G状态快照已过期`。

```cpp
const auto snapshot = cellular::ReadStatusSnapshot(path, 30s, now);
CHECK(snapshot.link_state == cellular::LinkState::kUnknown);
CHECK(snapshot.last_error == "5G状态快照已过期");
```

- [ ] **Step 2: 运行 C++ 测试确认失败**

先在 CMake 注册 `test_cellular_snapshot`，运行：

`cmake -S . -B build && cmake --build build --target test_cellular_snapshot`

Expected: FAIL，找不到 `cellular/cellular_snapshot.hpp`。

- [ ] **Step 3: 实现类型、读取和公开 JSON**

`StatusSnapshot` 使用 `std::optional` 表达所有可空字段，诊断项保持布尔值。读取
接口接受 `std::filesystem::file_time_type now` 以便测试 mtime，不使用不可信墙上
时间判断新鲜度。`BuildPublicTelemetryJson` 明确逐项复制公开字段，禁止复制
`diagnostics`。

- [ ] **Step 4: 写 RPICELL 五态兼容测试**

用同一组诊断位分别构造五种状态，断言 `ONLINE`/`DEGRADED` 的 bit0 为 1，其他
三态为 0；bit1–bit4 始终来自内部 diagnostics。保留现有 MAVLink 消息名、msgid、
时间和动态 system/component id 测试。

- [ ] **Step 5: 改为从快照映射并运行测试**

删除 `cellular_status.cpp` 对 `/sys/class/net`、`getifaddrs` 和路由表的直接探测；
保留 `LinkStatus`、`PackRpiCellularValue` 和 `BuildRpiCellularHeartbeat` 对外接口，
新增：

```cpp
LinkStatus FromSnapshot(const StatusSnapshot& snapshot);
```

Run: `cmake --build build --target test_cellular_snapshot test_cellular_status && ctest --test-dir build -R "cellular_(snapshot|status)" --output-on-failure`

Expected: PASS。

- [ ] **Step 6: 提交 C++ 快照消费层**

```bash
git add src/cellular/cellular_snapshot.hpp src/cellular/cellular_snapshot.cpp \
  src/cellular/cellular_status.hpp src/cellular/cellular_status.cpp \
  tests/test_cellular_snapshot.cpp tests/test_cellular_status.cpp CMakeLists.txt
git commit -m "feat: 使用统一5G状态快照生成RPICELL"
```

---

### Task 5: MQTT 遥测与主循环接入

**Files:**
- Modify: `src/config/app_config.hpp`
- Modify: `src/config/app_config.cpp`
- Modify: `tests/test_app_config.cpp`
- Modify: `src/payload/json_serializer.hpp`
- Modify: `src/payload/json_serializer.cpp`
- Modify: `tests/test_json_serializer.cpp`
- Modify: `src/main.cpp`
- Modify: `config/config.example.json`

**Interfaces:**
- Consumes: Task 4 的 `StatusSnapshot`、`ReadStatusSnapshot`、`BuildPublicTelemetryJson`。
- Produces: `payload::ToJson(state, school_name, cellular_snapshot)` 重载。

- [ ] **Step 1: 写 C++ 消费配置失败测试**

给 `CellularConfig` 增加默认：

```cpp
std::filesystem::path status_snapshot_path{"/run/cns-rpi/cellular_status.json"};
std::chrono::seconds status_snapshot_max_age{30};
```

测试老配置缺少这两个字段时使用默认值；显式空路径、包含 NUL、最大年龄小于
10 秒或大于 300 秒时返回 `kInvalidValue`。

- [ ] **Step 2: 实现配置解析并跑通测试**

配置 JSON 字段名为 `status_snapshot_path`、`status_snapshot_max_age_seconds`；二者
均为可选字段，并把相同默认值写入 `config/config.example.json`。Run:

`cmake --build build --target test_app_config && ctest --test-dir build -R app_config --output-on-failure`

Expected: PASS。

- [ ] **Step 3: 写遥测注入失败测试**

```cpp
auto json = payload::ToJson(state, "NNUTC", sample_cellular_snapshot);
REQUIRE(json["telemetry"].contains("cellular_5g"));
CHECK(json["telemetry"]["cellular_5g"]["link_state"] == "DEGRADED");
CHECK_FALSE(json["telemetry"]["cellular_5g"].contains("diagnostics"));
```

再用 `UNKNOWN` 快照断言所有公开字段存在且未知值为 `null`；保留一项旧的完整
遥测测试，确认新增字段之外的结构不变。

- [ ] **Step 4: 实现 serializer 重载**

保持原两参数接口供既有测试和调用者使用；新增：

```cpp
nlohmann::json ToJson(const state::TelemetryState& state,
                      const std::string& school_name,
                      const cellular::StatusSnapshot& cellular_status);
```

三参数版本先调用共享内部构建逻辑，再设置
`root["telemetry"]["cellular_5g"] = BuildPublicTelemetryJson(cellular_status)`。

- [ ] **Step 5: 主循环统一读取快照**

在发送 `RPICELL` 的周期读取快照并调用 `FromSnapshot`；在每次发布 MQTT 遥测前
重新读取快照并传给三参数 `ToJson`。两处使用相同路径和最大年龄配置，不再调用
`ProbeLink`。读取失败只产生 `UNKNOWN` 快照，不阻断串口和 MQTT 主循环。

- [ ] **Step 6: 运行相关测试并提交**

Run:

`cmake --build build --target test_app_config test_json_serializer test_cellular_status cns_rpi && ctest --test-dir build -R "app_config|json_serializer|cellular_status" --output-on-failure`

Expected: PASS。

Commit:

```bash
git add src/config/app_config.hpp src/config/app_config.cpp tests/test_app_config.cpp \
  src/payload/json_serializer.hpp src/payload/json_serializer.cpp \
  tests/test_json_serializer.cpp src/main.cpp config/config.example.json
git commit -m "feat: 上报5G链路质量遥测"
```

---

### Task 6: 部署契约、文档同步与全量验证

**Files:**
- Modify: `tests/test_cns_rpi_service.cpp`
- Modify: `scripts/deploy.sh`
- Modify: `docs/technical_docs/01_5G模块技术文档.md`
- Modify: `docs/V1设计文档.md`
- Modify: `docs/M7系统化部署设计.md`

**Interfaces:**
- Consumes: Tasks 1–5 的全部行为。
- Produces: 可幂等部署并能重启新守护进程的最终交付。

- [ ] **Step 1: 写 systemd 和部署契约失败测试**

扩展 `test_cns_rpi_service.cpp`，读取 `systemd/cellular-dialup.service` 和
`scripts/deploy.sh`，断言：

- 单元包含 `Type=simple`、`User=dcdw`、`RuntimeDirectory=cns-rpi`；
- `ExecStart` 使用 `/var/lib/cns-rpi/config.json`；
- 不包含 `RemainAfterExit=yes`；
- 部署脚本在安装并 `daemon-reload` 后，对已运行的 cellular 服务执行 restart，
  未运行时执行 start。

- [ ] **Step 2: 修改部署脚本并跑契约测试**

将当前无条件 `start --no-block` 改为与主服务一致的幂等分支：

```bash
if sudo systemctl is-active --quiet cellular-dialup.service; then
  sudo systemctl restart cellular-dialup.service
else
  sudo systemctl start cellular-dialup.service
fi
```

Run: `cmake --build build --target test_cns_rpi_service && ctest --test-dir build -R cns_rpi_service --output-on-failure`

Expected: PASS。

- [ ] **Step 3: 同步技术文档**

更新三份文档，明确：服务已从 oneshot 升级为常驻状态机；双目标经过白名单实测；
质量字段来源；恢复阶梯；持久配置和 `/run` 快照路径；STM32 无需变更；服务端
无需新增数据库字段。删除或标注所有“只开机拨号一次”“尚未采集质量”的过时表述。

- [ ] **Step 4: 运行格式和全量测试**

Run:

```bash
python3 -m unittest -v tests.test_cellular_link tests.test_cellular_dialup
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: Python 全部 PASS；C++ 构建成功；CTest `100% tests passed`；
`git diff --check` 无输出。

- [ ] **Step 5: 提交部署与文档**

```bash
git add tests/test_cns_rpi_service.cpp scripts/deploy.sh \
  docs/technical_docs/01_5G模块技术文档.md docs/V1设计文档.md \
  docs/M7系统化部署设计.md
git commit -m "docs: 同步5G自恢复部署与验收说明"
```

---

### Task 7: 树莓派实机验收

**Files:**
- Modify after confirmed results: `docs/technical_docs/01_5G模块技术文档.md`

**Interfaces:**
- Consumes: Task 6 的完整构建和部署结果。
- Produces: ARM 构建、真实 RM500U、白名单公网、MQTT 和 STM32 兼容的终验记录。

- [x] **Step 1: 推送或以 bundle 同步树莓派并构建**

树莓派 GitHub 5G 访问不稳定时沿用已验证的 Git bundle 同步方式。执行：

```bash
cmake -S /home/dcdw/cns_rpi -B /home/dcdw/cns_rpi/build
cmake --build /home/dcdw/cns_rpi/build -j2
ctest --test-dir /home/dcdw/cns_rpi/build --output-on-failure
```

Expected: ARM 平台构建成功，全部测试通过。

- [x] **Step 2: 更新只读持久配置并部署**

通过现有配置 helper 写入新增字段，禁止直接修改只读挂载。确认
`/var/lib/cns-rpi/config.json` 包含正式 APN 和双目标，再在根文件系统可持久写入的
维护窗口执行 `./scripts/deploy.sh`。

- [x] **Step 3: 验证正常在线和质量快照**

```bash
systemctl status cellular-dialup.service cns-rpi.service
cat /run/cns-rpi/cellular_status.json
ping -I usb0 -c 3 112.124.52.232
ping -I usb0 -c 3 119.29.29.29
journalctl -u cellular-dialup.service -n 100 --no-pager
```

Expected: cellular 服务为 `active (running)`；快照为 `ONLINE`，包含实测质量值；
日志只出现启动和状态变化，不按 10 秒刷屏。

- [x] **Step 4: 在用户配合下验证拔插恢复**

用户明确告知拔出 RM500U 后，确认快照变为 `present=false/OFFLINE`；用户插回后，
确认动态发现新的 AT 枚举号、自动拨号并在两轮成功后回到 `ONLINE`。不得在用户
未确认操作窗口时远程要求或假设硬件已拔插。

- [x] **Step 5: 验证遥测与 STM32 无感知**

从 MQTT 实际遥测确认存在 `telemetry.cellular_5g` 且没有 `diagnostics`；确认 STM32
仍收到 `RPICELL`，在线时 bit0=1。实机不主动破坏白名单或公网路由；持续断链与
`CFUN` 阶梯使用测试替身覆盖，若需真机阻断必须再次取得用户确认。

- [x] **Step 6: 记录终验事实并提交**

把 ARM 构建结果、RM500U 固件响应、服务状态、快照样例、双目标可达性、拔插恢复
耗时和 STM32 无感知结论写入技术文档。

```bash
git add docs/technical_docs/01_5G模块技术文档.md
git commit -m "docs: 记录5G链路自恢复实机验收结果"
```

---

## 最终完成条件

- Python 与 C++ 全量测试通过，ARM 树莓派构建通过。
- `cellular-dialup.service` 为常驻、单一 AT 口所有者，首次拨号和运行期恢复均正常。
- 双目标探测强制走 `usb0`，Wi-Fi 不造成误判。
- 快照按周期原子刷新，过期或损坏不影响主程序其他遥测。
- MQTT 包含全部约定公开字段且不包含内部 diagnostics。
- STM32 无需改动，仍按原 `RPICELL` 协议显示在线状态。
- 相关设计、技术和部署文档与最终代码及实机事实一致。
