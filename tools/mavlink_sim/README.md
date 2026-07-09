# MAVLink 模拟收发工具

在真机 STM32 固件的新 UART 链路还没接好之前，用 `send_frames.py` 模拟 STM32 → RPi
的串口发送，不依赖真机 STM32 就能验证/演示 RPi 侧（M2-M3c）的解码效果。**临时验证
工具，不是 cns_rpi 项目本身要交付的代码，不会被 CMake 构建**。

`receive_frames.py` 是 M2 的 C++ 解码层落地前的过渡验证脚本，**现在已经没用了**——
验证/演示收到的内容，直接在 RPi 上跑真正的 `cns_rpi` 可执行文件看它自己的解码打印
（`LogTelemetry`/`LogExtension`），不要再用这个 Python 脚本模拟解码。

## 依赖

[pymavlink](https://github.com/ArduPilot/pymavlink)（官方 MAVLink Python 库，用它
编码/解码帧，不手写 CRC/帧格式）+ `pyserial`（pymavlink 走串口连接时依赖它，不是
自动带的，需要单独装）：

```bash
python3 -m venv ~/.venvs/mavlink-sim
~/.venvs/mavlink-sim/bin/pip install pymavlink pyserial
```

## 用法

物理连接：开发机和 RPi 之间接一条真实的串口链路（具体用什么线材/适配器不限，
两端各自会出现一个串口设备路径，比如 `/dev/ttyUSB0`）。

**开发机**（模拟 STM32 发送）：

```bash
~/.venvs/mavlink-sim/bin/python3 tools/mavlink_sim/send_frames.py --port /dev/ttyUSB0 --baud 57600
```

**RPi**（跑真正的 cns_rpi 可执行文件，看它自己解码打印的内容）：

```bash
./build/cns_rpi config/config.json   # 串口设备路径以config里配置的为准，要跟send_frames.py的--port指向同一条链路
```

两端波特率必须一致；最终真实波特率以固件侧实际配置为准（`docs/V1设计文档.md` §11
的开放依赖项之一），`send_frames.py` 的默认 57600 只是测试值，可以用 `--baud` 改，
要跟 RPi 侧配置文件里的波特率一致。

## 现在模拟发送的消息

`send_frames.py` 每轮（默认 1Hz）发送：

- 标准遥测：`HEARTBEAT` / `GPS_RAW_INT` / `ATTITUDE` / `SYS_STATUS` /
  `BATTERY_STATUS`(id=0 电池1 + id=1 电池2) / `SCALED_PRESSURE`（2026-07-09 跟
  M4 官方通道切换同步：电池2 从自定义 `BAT2STAT` 改成官方 `BATTERY_STATUS(id=1)`，
  气压/温度从自定义字段改成官方 `SCALED_PRESSURE`）
- 扩展帧（`NAMED_VALUE_INT`）：`MODSTAT0`/`MODSTAT1`/`MOTOR12`/`MOTOR34`/
  `GNSS_SAT`/`HUMIDITY`/`LORASTAT`/`RIDSTAT`（2026-07-07 跟固件 `Formal_Framework`
  PR#1 同步：`ENVHUM`→`HUMIDITY`、单条 `MOTORPWM`→`MOTOR12`/`MOTOR34`，新增
  `LORASTAT`/`RIDSTAT` 两条 RPi 专属遥测）
- 扩展帧（`TUNNEL`）：告警表(`0x8001`)一行示例 + 日志增量(`0x8002`)心跳（不带日志条目）
- 身份帧（`OPEN_DRONE_ID_*`）：`BASIC_ID`/`LOCATION`/`SYSTEM`/`OPERATOR_ID`/`SELF_ID`
  五种全部覆盖，`BASIC_ID.uas_id` 用 `DEMO_VENDOR_ID` 演示值（不是真实设备 SN）

对应 `docs/V1设计文档.md` §4 里 RPi V1 要对接的上行消息范围，除了主从机专用的
`LORASUM`（跟 RPi 关系不大，故意不接）之外全部覆盖。故意损坏的帧（用于测试解码器
健壮性）暂未覆盖，需要时再加。
