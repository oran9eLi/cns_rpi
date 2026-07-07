# M4 JSON payload 构造 设计文档

版本：2026-07-07
状态：已批准，待写实施计划
里程碑：M4（`docs/V1设计文档.md` §10）——`state_store` 转 JSON，人工检查字段可读性。

## 1. 背景与范围

M3a/M3b/M3c 已经把 STM32 发来的所有 MAVLink 帧解码进 `state::TelemetryState`（原始刻度、不做单位换算，见 `state_store.hpp` 文件头）。M4 要把这份内部状态转成一份人类可读的 JSON，供人工核对字段含义/数值是否正确——这是 M5（MQTT 发布）的前置步骤，但 M4 本身不接 MQTT。

用户提供的草稿 `/home/oran9e/test.json` 给出了大致形状（`identity` 块 + 模块数组），但草稿里的 `school_name` 字段在 `state_store` 里没有对应数据来源（那是本机静态配置，不是 STM32 解码出来的），`modules` 数组把状态简化成了 `"active"/"inactive"`，跟实际的 0-6 枚举、14个模块不一致——这些差异已经跟用户逐条确认过，本文档是确认后的完整设计。

## 2. 文件布局与接口

- 新增 `src/payload/json_serializer.hpp/.cpp`（文件树已在 `docs/V1设计文档.md` 里预留这个位置）。
- 核心函数：
  ```cpp
  namespace payload {
  nlohmann::json ToJson(const state::TelemetryState& state, const std::string& school_name);
  }
  ```
  只传 `school_name` 这一个额外参数，不传整个 `config::AppConfig`——避免 `payload/` 反过来依赖 `config/`，保持"解码与消费者分离"的既定架构原则（`payload/` 已经依赖 `state/`，不需要再依赖 `config/`）。
- `config/app_config.hpp` 新增：
  ```cpp
  struct IdentityConfig {
    std::string school_name;
  };
  ```
  `AppConfig` 新增 `IdentityConfig identity;` 字段，`LoadAppConfig` 增加对 `"identity"."school_name"` 的解析（缺失时按现有 `ConfigError::kMissingField` 规则处理，参照 `serial`/`mqtt` 字段的现有解析方式）。
- `config/config.example.json` 新增：
  ```json
  "identity": {
    "school_name": "NNUTC"
  }
  ```

## 3. 顶层 JSON 结构

```json
{
  "identity": {
    "vendor_id": "DCDWCNS1XXXXXXXXXXXX",
    "dcdw_label": "DCDW-001",
    "rpi_serial": "...",
    "school_name": "NNUTC"
  },
  "telemetry": {
    "heartbeat": {...},
    "gps": {...},
    "gnss_sat": {...},
    "attitude": {...},
    "global_position": {...},
    "sys_status": {...},
    "battery": {...},
    "battery2": {...},
    "pressure": {...},
    "humidity": {...},
    "motor": {...},
    "lora": {...},
    "remote_id": {...}
  },
  "modules": [
    {"name": "GNSS", "status": "ONLINE"}
  ],
  "alarms": {...},
  "logs": {...},
  "drone_id": {
    "basic_id": {...},
    "location": {...},
    "system": {...},
    "operator_id": {...},
    "self_id": {...}
  }
}
```

规则：
- key 一律 snake_case（跟 `config.json` 现有风格一致）。
- `identity.vendor_id`/`dcdw_label`/`rpi_serial` 各自独立按 `state_store` 里对应的 `std::optional<std::string>` 省略；`school_name` 恒定存在（静态配置，不是 optional）。
- `telemetry.*`/`drone_id.*` 每个二级 key 按对应 `TelemetryState` 字段的 `std::optional` 独立省略——某条 MAVLink 消息没收到过，JSON 里就没有这个 key，不输出 `null`。
- `modules`（整个数组）/`alarms`/`logs`（整个对象）在 `module_status`/`alarm_table`/`message_log` 从未被写入过时，整个顶层 key 省略。
- 没有"凑齐才输出"的门槛——每收到一帧就对当前完整快照重新序列化一次；哪些字段有值完全取决于目前为止收到过哪些 MAVLink 消息。

## 4. 枚举/状态字符串化范围

- 模块状态（0-6，`Px4Lite_State_t`，V1设计文档§4.1）→ 字符串：`UNINITIALIZED`/`STARTING`/`ONLINE`/`DEGRADED`/`OFFLINE`/`FAILED`/`DISABLED`。
- 模块编号（0-13，`Px4Lite_ModuleId_t`）→ 名称：`GNSS`/`IMU`/`BARO`/`BATTERY`/`LORA`/`5G`/`STORAGE`/`REMOTE_ID`/`DISPLAY`/`CONTROL`/`ALARM`/`SYSTEM`/`ESTIMATOR`/`BUSINESS`。
- `lora.link_state`（跟模块状态复用同一个 `Px4Lite_State_t` 枚举，见 `state_store.hpp` 里 `LoraStatus` 的注释）→ 复用上面同一张字符串表。
- **标准 MAVLink 官方枚举/位图字段保持原始数字，不转字符串**：`heartbeat.type/autopilot/base_mode/system_status`、`gps.fix_type`、`battery.type/mode/charge_state/battery_function/fault_bitmask`、`sys_status` 的三组传感器位图、以及所有 `OPEN_DRONE_ID_*` 里的 `id_type/ua_type/status/height_reference/*_accuracy/classification_type/category_eu/class_eu/operator_location_type` 等 ODID 官方枚举。

  理由：这些是上游协议自带的几十种取值/位标志，翻译成字符串需要维护一整套独立的 `MAV_STATE`/`MAV_TYPE`/`MAV_AUTOPILOT`/`GPS_FIX_TYPE`/`MAV_BATTERY_*`/`MAV_ODID_*` 映射表，属于比 M4 验收范围（人工核对自定义字段的可读性）明显更大的独立工作；且这些是标准协议代码本身可查，跟只有本仓库知道含义的 0-6 模块枚举性质不同。

## 5. 单位换算表

| 字段 | 原始刻度 | 换算后 | 公式 |
|---|---|---|---|
| `gps.lat/lon`、`global_position.lat/lon`、`drone_id.location.latitude/longitude`、`drone_id.system.operator_latitude/longitude` | degE7 (int32) | 度 (double) | `/1e7` |
| `gps.alt`、`global_position.alt/relative_alt` | mm (int32) | 米 (double) | `/1000.0` |
| `gps.vel`、`global_position.vx/vy/vz` | cm/s | m/s (double) | `/100.0` |
| `gps.cog`、`global_position.hdg`、`gps.yaw`、`drone_id.location.direction` | cdeg | 度 (double) | `/100.0` |
| `drone_id.location.speed_horizontal/speed_vertical` | cm/s | m/s (double) | `/100.0` |
| `attitude.roll/pitch/yaw`（含对应的 `*speed`） | rad (float) | 度 (double) | `*180/π` |
| `pressure.press_abs/press_diff` | hPa (float) | hPa（已是人类单位） | 直接透传 |
| `pressure.temperature`、`pressure.temperature_press_diff`、`battery.temperature` | cdegC | °C (double) | `/100.0` |
| `sys_status.voltage_battery`、`battery2.voltage_mv` | mV | V (double) | `/1000.0` |
| `sys_status.current_battery`、`battery.current_battery` | cA | A (double) | `/100.0` |
| `sys_status.load`、`sys_status.drop_rate_comm` | d%/c%（×10） | % (double) | `/10.0` |
| `battery.voltages[]`/`voltages_ext[]` | mV | V 数组 (double) | `/1000.0`（未使用槽位见下方哨兵规则） |
| `battery.energy_consumed` | hJ | J (double) | `*100.0` |
| `battery.current_consumed`/`time_remaining`/`battery_remaining` | mAh/s/% | 原样 | 无需换算（已是人类单位） |
| `lora.loss_rate_x10`、`humidity.relative_humidity_x10` | ×10 | % (double) | `/10.0` |
| `motor.duty_percent[]`/`speed_level` | 已是 % | 原样 | 无 |
| `drone_id.location.altitude_barometric/altitude_geodetic/height`、`drone_id.system.area_ceiling/area_floor/operator_altitude_geo` | 已是米 (float) | 原样 | 无（哨兵值见下） |
| `remote_id.last_success_ms` | ms（开机时刻） | 原样 | 无（没有更合适的单位） |

**哨兵值 → `null`**：MAVLink 用特殊值表示"未提供"，换算前先判断是否命中哨兵值，命中则该字段输出 `null`（不换算出无意义的数字）：
- `gps.eph/epv/vel/cog` = `UINT16_MAX`
- `sys_status.current_battery`/`battery.current_battery`/`battery.battery_remaining` = `-1`
- `battery.voltages[]`/`voltages_ext[]` 里等于 `UINT16_MAX`（或 `voltages_ext` 里等于 `0`，按官方注释是"不支持"）的槽位
- `drone_id.location`/`drone_id.system` 里等于 `-1000.0f` 的高度类字段、`direction`=`36100`、`speed_horizontal`=`25500`、`speed_vertical`=`6300`

## 6. `drone_id`/`alarms`/`logs` 细节

- `drone_id.basic_id/location/system/operator_id/self_id`：按第4、5节规则做枚举保留/单位换算。`uas_id`/`operator_id`/`description` 这几个 `char[]` 字段转成去除尾部空字符的字符串；`id_or_mac`（我们自己广播时恒为全零，字段本身是"仅用于接收其他飞行器数据时"）转成十六进制字符串——20字节对应40个十六进制字符，全零时为40个`'0'`组成的字符串。
- `alarms` = `{"ver": ..., "entries": [{"source_id","fault_code","severity","active","age_s"}, ...]}`，按 `active_count` 截断（`fault_code`/`source_id`/`severity` 业务含义未知，文档已注明，保持原始数字）。
- `logs` = `{"latest_seq": ..., "entries": [{"sequence","message_id","time":"HH:MM:SS","severity"}, ...]}`，按 `count` 截断；`time` 由 `time_hhmmss` 三元组拼成 `"HH:MM:SS"` 字符串。

## 7. main.cpp 集成

新增 `LogJsonPayload(const state::TelemetryState& state, const std::string& school_name)`，跟现有 `LogTelemetry`/`LogExtension` 同一个调用点（每处理完一帧调用一次），内部调用 `payload::ToJson(...).dump(2)` 打印到控制台，方便真机演示时人工核对。

## 8. 测试范围

`tests/test_json_serializer.cpp`：
- 空 `TelemetryState` → 输出只剩 `identity.school_name`，其余顶层 key 全部不存在。
- 每组字段（`telemetry.*`/`modules`/`alarms`/`logs`/`drone_id.*`）独立填充后，对应 key 存在且数值正确；未填充的字段仍然不存在。
- 单位换算抽样核对：`gps.lat/lon`、`attitude.roll`（弧度转角度）、`pressure.temperature`、`sys_status.voltage_battery`。
- 哨兵值 → `null`：至少覆盖 `gps.eph=UINT16_MAX`、`sys_status.current_battery=-1`、`drone_id.location.height=-1000.0f` 三种情况。
- 模块状态：14 项都输出正确的 `name`/`status` 字符串，含未收到过状态的模块（值为0，应显示 `UNINITIALIZED`，这是零初始化后的合法语义，不是异常）。
- `alarms.entries`/`logs.entries` 数组按 `active_count`/`count` 截断，含 0 条、中间值、满 14/9 条三种边界。
- `uas_id`/`operator_id`/`description` 去除尾部空字符后的字符串正确；`id_or_mac` 十六进制字符串格式正确。

## 9. 全局约束（供实施计划引用）

- `state_store` 层"存原样不做单位换算"的既定原则不变——所有单位换算只发生在 `payload/json_serializer` 这一层。
- 标准 MAVLink/ODID 官方枚举保持原始数字，不维护枚举名映射表；仅本项目自定义的模块状态枚举（含 `LORASTAT.link_state` 复用的同一枚举）转字符串。
- 未收到过的字段省略 JSON key，不输出 `null`；`null` 只用于"消息收到过，但该字段命中协议定义的哨兵值(表示未知/不支持)"这一种情况。
- key 一律 snake_case。
- 每个任务完成后运行对应测试，全绿才算完成；任务结束提交一次。
