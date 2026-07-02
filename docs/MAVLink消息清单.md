# RPi 需要对接的 MAVLink 消息清单

版本：2026-07-01
状态：草案，待确认
说明：本文档细化 `docs/V1设计文档.md` §4，把 RPi 需要收发的 MAVLink 消息按类型、频率、数据内容列全，供你核对是否需要调整。

## 0. 数据来源与一个重要限制

频率数据来自固件仓库两个**现有**调度表：

- `Framework/Src/px4lite_mavlink_tx.c` 的 `s_mav_tx_catalog[]`——这是 **LoRa 链路**（USART3）的遥测发送调度。
- `Framework/Src/px4lite_remoteid_tx.c` 的 `s_remoteid_catalog[]`——这是 **RemoteID 链路**（UART4→ESP32-S3）的身份发送调度。

**这两条链路目前都不是发给 RPi 的**（`docs/V1设计文档.md` §2 已经澄清过）。RPi 的新 UART 还没有自己的调度表，本文档列出的频率是"现有实现的参考值"，不是"RPi 链路的最终频率"——LoRa 是低速无线链路，这些周期是按 LoRa 带宽预算压出来的，RPi 是直连有线 UART，带宽宽裕得多，频率大概率可以定得更高。**消息类型和字段编码可以直接复用，频率需要固件侧重新评估。**

## 1. 遥测类（标准 MAVLink 消息，来自 LoRa 调度表）

| 消息 | 当前(LoRa)频率 | 关键字段 |
|---|---|---|
| `HEARTBEAT` | 1 Hz（1000ms） | `type`/`autopilot`/`base_mode`/`custom_mode`/`system_status` |
| `GPS_RAW_INT` | 0.25 Hz（4000ms） | `fix_type`/`lat`/`lon`/`alt`/`eph`/`epv`/`vel`/`cog`/`satellites_visible` |
| `ATTITUDE` | 0.5 Hz（2000ms） | `roll`/`pitch`/`yaw`/`rollspeed`/`pitchspeed`/`yawspeed` |
| `GLOBAL_POSITION_INT` | 0.25 Hz（4000ms） | `lat`/`lon`/`alt`/`relative_alt`/`vx`/`vy`/`vz`/`hdg` |
| `SYS_STATUS` | 0.125 Hz（8000ms） | `voltage_battery`/`current_battery`/`battery_remaining`/各类 sensor present/enabled/health 位图 |
| `BATTERY_STATUS` | 0.125 Hz（8000ms） | 电池详细状态（cell 电压等标准字段） |
| `SCALED_PRESSURE` | 0.125 Hz（8000ms） | `press_abs`/`press_diff`/`temperature` |
| `STATUSTEXT` | ~0.067 Hz（15000ms） | `severity`/`text`（人类可读状态文本） |

## 2. 扩展帧（借用 `NAMED_VALUE_INT`/`TUNNEL`，字段字典见 `docs/V1设计文档.md` §4.1/4.2）

| name / payload_type | 当前(LoRa)频率 | 内容 |
|---|---|---|
| `MODSTAT0` / `MODSTAT1` | 各 0.125 Hz（8000ms），两者交替发送 | 14 个模块的状态位图（4bit/模块） |
| `BAT2STAT` | 0.125 Hz（8000ms） | 电压(mV)/电量(%)/低电压标志 |
| `MOTORPWM` | 0.2 Hz（5000ms） | 4 个电机占空比(%) |
| `GNSS_SAT` | 0.1 Hz（10000ms） | GPS/北斗可见数与使用数 |
| `ENVHUM` | 0.125 Hz（8000ms） | 相对湿度 |
| `TUNNEL`（告警表，`payload_type=0x8001`） | 0.125 Hz（8000ms） | 当前活动告警表（最多14行） |
| `TUNNEL`（日志增量，`payload_type=0x8002`） | 0.125 Hz（8000ms）增量，每 30000ms 全量重发一次 | 结构化日志条目（最多9条/帧） |
| `LORASUM` | 1 Hz（1000ms） | LoRa 远程查看租约摘要——**LoRa 主从机专用，不建议接入 RPi** |

## 3. 身份类（`OPEN_DRONE_ID_*`，来自 RemoteID 调度表）

RPi 是"汇总遥测+身份数据的总入口"（`docs/V1设计文档.md` §1），所以这几条也要接进 RPi 的新链路——**但现在这几条走的是 UART4→ESP32-S3，不是发给 RPi，接入 RPi 需要固件侧新增一份独立调度，不是简单复用现有代码。**

| 消息 | 当前(RemoteID链路)频率 | 内容 |
|---|---|---|
| `HEARTBEAT`（RemoteID 链路自己的心跳，和第1节的心跳是两个独立实例，不是同一条） | 1 Hz（1000ms） | 同上 |
| `OPEN_DRONE_ID_BASIC_ID` | 1 Hz（1000ms） | UAS ID 等身份基本信息 |
| `OPEN_DRONE_ID_LOCATION` | 1 Hz（1000ms） | 位置、速度、高度 |
| `OPEN_DRONE_ID_SYSTEM` | 1 Hz（1000ms） | 系统/操作区域信息 |
| `OPEN_DRONE_ID_OPERATOR_ID` | 1 Hz（1000ms） | 操作者身份 |
| `OPEN_DRONE_ID_SELF_ID` | 0.2 Hz（5000ms） | 自定义描述文本 |

## 4. 命令下行（非周期，事件驱动）

- `COMMAND_LONG`：RPi → STM32，见 `docs/V1设计文档.md` §5
- `COMMAND_ACK`：STM32 → RPi，命令执行结果

命令是事件驱动的，不适用"频率"这个概念，来一条处理一条。

## 5. 开放问题

- 第1、2节的频率是否直接沿用 LoRa 现有值，还是给 RPi 链路重新定一套（建议：直连 UART 带宽够，可以定得更高，尤其 `GNSS_SAT`/`STATUSTEXT` 这种现在压得很低的）——待你决定
- `OPEN_DRONE_ID_*` 接入 RPi 新链路，需要固件侧新写一份调度表（不是从 RemoteID 链路"顺便转发"），这个工作量需要你评估
- 两条 `HEARTBEAT`（telemetry 的和 RemoteID 的）要不要在 RPi 新链路上合并成一条，还是保留成两条独立心跳——待你决定
