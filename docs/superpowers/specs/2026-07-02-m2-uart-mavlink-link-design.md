# M2：UART/MAVLink 收发帧层 设计

日期：2026-07-02
状态：草案，待用户审阅
对应里程碑：`docs/V1设计文档.md` §10 M2

## 1. 背景

`src/mavlink/` 已经 vendor 了固件仓库同一份官方 MAVLink v2 头文件（commit `f76115f`）。M2 的目标是在这份头文件基础上，用官方 `mavlink_frame_char_buffer()` 把串口原始字节流和完整 MAVLink 帧接起来——只验证帧完整性和 CRC，双向都要跑通，不解帧内内容（帧内容解码是 M3 的事）。

设计文档原本把 M2 标了一条阻塞项："需固件侧先分配新 UART（口号/波特率）并能收发真实 HEARTBEAT"。本次设计过程中确认：

- 物理链路：开发板上原本用于调试输出的 miniUSB 口（板载 CH340/CH341 家族桥接芯片，跳线帽选通 UART1）现在划给 RPi 链路用，固件侧以后直接往 UART1 写数据即可。miniUSB 口通过标准 miniUSB转A 数据线直接接树莓派 USB-A 口，不需要任何跳线/杜邦线桥接。
- 已在真实树莓派（`dcdw@192.168.11.4`）上验证：插上后 RPi 侧稳定识别为 `/dev/ttyUSB0`（驱动 `ch341-uart`，`idVendor=1a86, idProduct=7523`），且**不依赖固件应用代码**——桥接芯片本身就能枚举，固件还没写 MAVLink 部分时插拔也能在 RPi 上看到设备。
- 固件侧真正把 HEARTBEAT 从 UART1 发出来这件事还没做（"没写/没跑通"），所以本设计里 C++ 层的验证分两条腿：不依赖硬件的单元测试，以及用现有 `tools/mavlink_sim/` 假扮 STM32 在这条已验证的物理链路上做人工验证——不等真固件也能把 M2 的软件部分做完并验证。

## 2. 架构

新增两个类，职责分层，符合 CLAUDE.md 里"帧同步交给官方库，不自造协议解析"的选型：

```
uart/serial_port.hpp/.cpp   —— 纯 termios 封装：开设备、配置 8N1/raw 模式/波特率、阻塞读写字节。
                                不知道 MAVLink 是什么，只知道"一个字符设备路径"。

uart/mavlink_link.hpp/.cpp  —— 包一层 serial_port，内部维护一个 mavlink_status_t：
                                - receive_message() -> std::optional<mavlink_message_t>
                                  读可用字节喂给官方 mavlink_frame_char_buffer()，凑齐一帧
                                  （CRC 由官方库内部校验）就返回，没凑齐返回空，不阻塞轮询方。
                                - send_message(const mavlink_message_t&) -> bool
                                  官方 encode 函数打包+送 CRC，写入串口。
```

`mavlink_link` 不解析 `mavlink_message_t` 内部字段——M2 的边界就卡在"拿到一条通过 CRC 校验的完整帧"为止，M3 的 `protocol/telemetry_decoder.cpp` 等模块才会去拆具体消息类型和字段。

错误处理：`serial_port::open()`/`mavlink_link` 的可能失败路径（设备不存在、权限不足、串口配置失败）用 `std::expected<T, UartError>` 表达，不用异常，和 §7 技术选型定的约定一致。

## 3. 配置

现在就上 JSON 配置文件（不用命令行参数占位）：

- `config/config.example.json`：
  ```json
  {
    "serial": { "device": "/dev/ttyUSB0", "baud": 115200 },
    "mqtt": { "broker_host": "192.168.1.100", "broker_port": 1883, "client_id": "cns-rpi",
              "username": "", "password": "", "topic_prefix": "cns_rpi", "qos": 1, "keepalive_seconds": 60 },
    "logging": { "level": "info", "file": "" }
  }
  ```
  - `serial.*`：**真实值**，已在真机上验证（见第1节）。
  - `mqtt.*`/`logging.*`：占位值，对应模块（M5/日志接入）实现时再定稿，`mqtt.topic_prefix` 尤其还会变——topic 命名方案本身在 `docs/V1设计文档.md` §9 还没定稿。
  - 身份字段（`box_label`/`vendor_id`等）**不放在这份配置里**：根据 `docs/设备标识符.md` 的设计，这些字段是运行时从 STM32 上报的，不是 RPi 本地配置项，避免两处数据不一致时不知道哪个是权威值。
- `src/config/app_config.hpp/.cpp`：用 `nlohmann/json`（apt 包 `nlohmann-json3-dev`，header-only，不需要像 MAVLink 头文件那样手动 vendor）读取上述文件，字段缺失/文件不存在/解析失败用 `std::expected` 报错。M2 阶段只消费 `serial.device`/`serial.baud` 两个字段，其余字段解析出来但暂不使用（对应模块实现时再接入）。
- `scripts/install_deps.sh` 加一行安装 `nlohmann-json3-dev`。

## 4. `main.cpp`：验证双向的最小闭环

读配置 → 用 `serial.device`/`serial.baud` 开 `mavlink_link` → 循环：

- 收到完整帧（`receive_message()` 非空）：打日志（msg id / len / sysid，不解内容）。
- 定期（比如 1Hz）发一条自己的 `HEARTBEAT` 出去：证明发送方向也通，这也是以后固件联调时能直接看到"RPi 在线"的信号，不用等 M6 命令链路才有反向流量。

这就是 M2 要交付的状态：空壳但两个方向都用官方库跑通，帧内容留给 M3。

## 5. 测试

- `tests/test_mavlink_link.cpp`（CTest）：不依赖硬件，字节序列用官方 encode 函数现造。覆盖：
  - 完整合法帧
  - CRC 损坏帧（篡改payload或crc字节）
  - 一帧被拆成两次 `read()`（模拟串口一次读不全的情况）
  - 合法帧前混入垃圾字节（模拟上电瞬间线路噪声/半帧）
- 物理链路人工验证（不进 CI，完成实现后手动跑一遍给用户看结果）：用现有 `tools/mavlink_sim/send_frames.py` 在已验证通的 miniUSB↔USB-A 物理链路上假扮 STM32 发送已知消息，确认新写的 C++ 接收端在真实链路上能收对；用 `receive_frames.py` 确认 C++ 发送端发出的帧能被正确解码。等固件侧 UART1 真正跑起来发 HEARTBEAT 后，这一步换成真实固件做一次端到端复核。

## 6. 影响范围 / 后续同步

- `docs/V1设计文档.md` §11 "STM32 侧新 UART 的口号、波特率" 这条开放依赖，在本设计实现落地时一并改成已解决状态（口号=UART1 经板载桥接芯片，波特率=115200，均已验证），按协作规则 §7 与代码改动同一次提交同步。
- 本设计不改变 `docs/V1设计文档.md` §8 目录结构（`uart/`、`config/` 已经在结构里预留），不改变架构原则（§3 解耦仍成立：`uart/` 只产出校验过的完整帧，不关心谁消费）。
