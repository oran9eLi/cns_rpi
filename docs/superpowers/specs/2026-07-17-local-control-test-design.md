# 本地飞控下发测试工具设计

状态：已确认并实施
日期：2026-07-17  
适用范围：树莓派本地绕过 MQTT/服务器，直接验证 STM32 控制链路

## 1. 目标

新增独立命令行工具 `cns_control_test`。工具接收与主程序 MQTT
`control/set` 完全相同的 JSON，复用生产代码完成 JSON 校验、MAVLink 编码、
STM32 端点识别、UART 下发、`COMMAND_ACK` 处理和 ACK JSON 构造。

工具只替换消息入口和回执出口：输入来自本地命令行，结果输出到终端；不连接
MQTT，不依赖服务器路由，不修改 `config.json`，不改变 `cns_rpi` 主程序行为。

## 2. 使用方式

仅解析、编码并打印，不访问串口：

```bash
./build/cns_control_test config/config.json \
  '{"command_id":"local-001","command":"set_motor_pwm","parameters":{"pwm_us":[1500,1500,1500,1500]}}'
```

真实下发到 STM32：

```bash
./build/cns_control_test --send config/config.json \
  '{"command_id":"local-001","command":"set_motor_pwm","parameters":{"pwm_us":[1500,1500,1500,1500]}}'
```

起飞、降落和急停继续使用同一 JSON 契约：

```json
{"command_id":"local-002","command":"takeoff","parameters":{}}
```

工具一次只处理一条命令，执行结束后退出。JSON 既可以直接放在命令行参数中，
也可以使用 `--file <path>` 从文件读取；两种输入方式互斥。

## 3. 安全边界

- 默认是 dry-run，不打开 UART，不发送控制帧。
- 只有显式提供 `--send` 才执行真实下发。
- 真实发送前在标准错误输出打印命令、四路 PWM 和目标 STM32 端点。
- 工具不绕过现有 PWM `1000..2000 us`、四路数组和无参数命令校验。
- 工具不抢占主程序串口；`cns_rpi` 已占用串口时打开失败并退出。
- 文档明确要求进行电机相关测试前拆除桨叶或断开动力。
- 不在生产主程序中增加隐藏 topic、信号或调试后门。

## 4. 共用业务链路

```text
命令行 JSON / JSON 文件
  -> control_command::Parse()
  -> ControlTransaction::Submit()
  -> control_command::EncodeCommandLong()
  -> uart::MavlinkLink::SendMessage()
  -> STM32 执行
  -> uart::MavlinkLink::ReceiveMessage()
  -> control_command::IsExpectedCommandAck()
  -> ControlTransaction::HandleMavlinkAck()
  -> ACK JSON 输出到 stdout
```

dry-run 执行到 `EncodeCommandLong()` 为止，解码生成的 `COMMAND_LONG` 后输出目标、
command ID 和 7 个参数，不调用 `MavlinkLink::Open()`。

真实发送模式先打开 `config.json` 中的 STM32 串口，再等待符合固件事实的心跳：

- `HEARTBEAT.type = MAV_TYPE_ONBOARD_CONTROLLER(18)`；
- USART6 帧头 `compid = 193`；
- 动态学习 `sysid`，合法范围 `1..250`；
- RPi 以“已学习的动态 `sysid` / `compid=191`”发送
  `COMMAND_LONG`；
- ACK 必须来自已学习的 `sysid/193`，并定向回填
  “同一动态 `sysid` / `compid=191`”。

## 5. 时间与状态机

- 等待 STM32 HEARTBEAT 最长 5 秒；超时不发送命令。
- 命令发送后使用现有 `ControlTransaction`，基础 ACK 超时为 2 秒。
- 收到 `MAV_RESULT_IN_PROGRESS` 时输出一行进度 ACK，并按现有状态机刷新超时。
- 收到终态 ACK 时输出最终 JSON 并退出。
- 超时时输出与主程序相同的 timeout ACK JSON。
- 工具单次运行只处理一条命令，因此不持久化 completed cache，也不承担跨进程幂等；
  操作者重复运行 `--send` 会生成一次新的真实下发，必须自行确认安全。

## 6. 输出与退出码

标准输出只写机器可读 JSON；诊断、目标端点和安全提示写标准错误输出。

| 退出码 | 含义 |
|---:|---|
| `0` | dry-run 成功，或 STM32 最终返回 `MAV_RESULT_ACCEPTED` |
| `1` | JSON/参数被拒绝，STM32 返回失败终态，或等待 ACK 超时 |
| `2` | 命令行参数、配置文件或输入文件错误 |
| `3` | UART 打开/写入失败，或等待 STM32 HEARTBEAT 超时 |

输出 ACK 字段复用 `BuildRejectedAck()`、`BuildMavlinkAck()` 和
`BuildTimeoutAck()`，确保与 MQTT `control/ack` 的业务字段一致。dry-run 额外输出：

```json
{
  "status": "dry_run",
  "command_id": "local-001",
  "command": "set_motor_pwm",
  "mavlink_command": 31013,
  "params": [1500, 1500, 1500, 1500, 0, 0, 0]
}
```

## 7. 文件结构

| 文件 | 职责 |
|---|---|
| `src/control_test/control_test_cli.hpp/.cpp` | 参数解析、输入文件读取、dry-run JSON 构造 |
| `src/control_test/main.cpp` | 打开配置/UART、等待端点、发送、等待 ACK、输出和退出码 |
| `tests/test_control_test_cli.cpp` | 参数组合、文件/直接 JSON 互斥、dry-run payload 测试 |
| `CMakeLists.txt` | 新增 `cns_control_test` 和测试目标 |
| `docs/本地飞控下发测试工具.md` | 操作、安全和真机联调说明 |

生产业务规则继续保留在 `control_command`、`ControlTransaction`、
`control_endpoint` 和 `MavlinkLink` 中，测试工具不得复制这些规则。

## 8. 自动化验证

- 直接 JSON 和 `--file` 参数解析。
- 缺少输入、重复输入、未知选项和 `--send` 位置无关。
- PWM、起飞、降落、急停继续通过现有 `test_control_command` 验证。
- dry-run 不打开串口，并正确输出 MAVLink command/参数。
- endpoint、ACK 来源和目标继续通过 `test_control_endpoint` 验证。
- `IN_PROGRESS`、终态和超时继续通过 `test_control_transaction` 验证。
- 完整构建和全部 CTest 通过。

## 9. 真机验收

1. 停止占用 STM32 UART 的 `cns_rpi` 服务。
2. 不带 `--send` 运行四类命令，确认只打印 dry-run JSON，STM32 无动作。
3. 拆除桨叶或断开电机动力。
4. 带 `--send` 发送安全 PWM 值，确认工具学习到动态 `sysid/193`。
5. 确认 STM32 只执行一次，并返回与主程序一致的 ACK JSON。
6. 保持 `cns_rpi` 占用串口时运行工具，确认工具失败退出而不是抢占串口。
7. 断开 STM32 或屏蔽 HEARTBEAT，确认 5 秒后退出且没有发送控制命令。

真机验收需要树莓派连接 STM32；自动化测试和 dry-run 不依赖硬件。
