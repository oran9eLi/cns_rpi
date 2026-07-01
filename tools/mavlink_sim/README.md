# MAVLink 模拟收发工具

在 M2（UART/MAVLink 收发帧层）真正实现前，用这两个脚本模拟 STM32 → RPi 的串口链路，
验证物理连接和帧内容，不依赖真机 STM32。**临时验证工具，不是 cns_rpi 项目本身要交付
的代码，不会被 CMake 构建**；等 M2 的 C++ 解码层写好后，`receive_frames.py` 就没用了，
换成真正的实现来验证。

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

**RPi**（临时接收解码打印，验证收到的内容）：

```bash
~/.venvs/mavlink-sim/bin/python3 tools/mavlink_sim/receive_frames.py --port /dev/ttyUSB0 --baud 57600
```

两端波特率必须一致；最终真实波特率以固件侧实际配置为准（`docs/V1设计文档.md` §11
的开放依赖项之一），这里的 57600 只是默认测试值，可以用 `--baud` 改。

## 现在模拟发送的消息

`send_frames.py` 每轮（默认 1Hz）发送：`HEARTBEAT` / `GPS_RAW_INT` / `ATTITUDE` /
`SYS_STATUS` / `NAMED_VALUE_INT`（name=`REMOTE_STATUS`），对应 `docs/V1设计文档.md`
§4 里 RPi V1 要对接的上行消息范围的一个子集。`OPEN_DRONE_ID_*` 身份帧、`TUNNEL`
扩展帧、故意损坏的帧（用于测试解码器的健壮性）暂未覆盖，需要时再加。
