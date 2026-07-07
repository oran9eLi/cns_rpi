# M4 JSON payload 构造 设计文档

版本：2026-07-07
状态：已批准，待写实施计划
里程碑：M4（`docs/V1设计文档.md` §10）——`state_store` 转 JSON，人工检查字段可读性。

## 1. 背景与范围

M3a/M3b/M3c 已经把 STM32 发来的所有 MAVLink 帧解码进 `state::TelemetryState`（原始刻度、不做单位换算，见 `state_store.hpp` 文件头）。M4 要把这份内部状态转成一份人类可读的 JSON，供人工核对字段含义/数值是否正确——这是 M5（MQTT 发布）的前置步骤，但 M4 本身不接 MQTT。

用户提供的草稿 `/home/oran9e/test.json` 给出了大致形状（`identity` 块 + 模块数组），但草稿里的 `school_name` 字段在 `state_store` 里没有对应数据来源（那是本机静态配置，不是 STM32 解码出来的），`modules` 数组把状态简化成了 `"active"/"inactive"`，跟实际的 0-6 枚举、14个模块不一致——这些差异已经跟用户逐条确认过，本文档是确认后的完整设计。

**重要背景**：这套实训箱是**固定不动的教学用主控箱**，不是真实飞行的无人机——不会飞、没有人驾驶。MAVLink/OPEN_DRONE_ID 协议里大量"飞行运动学"字段（速度、编队区域等）对这个箱子没有实际意义，因此本设计里 JSON 输出**不包含**以下几类字段（`state_store` 解码层不受影响，仍然原样存储官方结构体的全部字段——这是消费层/M4 一层的取舍，不改解码层）：
- `global_position` 的 `vx`/`vy`/`vz`（速度分量）、`relative_alt`（相对起飞点高度——箱子没有"起飞"这回事）
- `gps` 的 `vel`/`cog`/`yaw`（地速/航迹方向/GPS偏航角）及其对应的不确定度 `vel_acc`/`hdg_acc`
- `drone_id.location` 的 `speed_horizontal`/`speed_vertical`/`direction`/`height`，以及描述这几个字段的"孤儿元数据" `height_reference`/`speed_accuracy`
- `drone_id.system` 的 `area_ceiling`/`area_floor`/`area_count`/`area_radius`（编队/多机场景专用，这个箱子是单机）

`attitude`（姿态角/角速度）保留——这个字段来自箱子上真实的 MPU6050 传感器，箱体本身会有姿态变化（如倾斜/震动），不是摆设。

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
- **标准 MAVLink 官方枚举/位图字段保持原始数字，不转字符串**：`heartbeat.type/autopilot/base_mode/system_status`、`gps.fix_type`、`battery.type/mode/charge_state/battery_function/fault_bitmask`、`sys_status` 的三组传感器位图、以及所有 `OPEN_DRONE_ID_*` 里的 `id_type/ua_type/status/*_accuracy/classification_type/category_eu/class_eu/operator_location_type` 等 ODID 官方枚举（`height_reference` 随 `height` 字段一起删除，见第1节）。

  理由：这些是上游协议自带的几十种取值/位标志，翻译成字符串需要维护一整套独立的 `MAV_STATE`/`MAV_TYPE`/`MAV_AUTOPILOT`/`GPS_FIX_TYPE`/`MAV_BATTERY_*`/`MAV_ODID_*` 映射表，属于比 M4 验收范围（人工核对自定义字段的可读性）明显更大的独立工作；且这些是标准协议代码本身可查，跟只有本仓库知道含义的 0-6 模块枚举性质不同。

## 5. 单位换算表

| 字段 | 原始刻度 | 换算后 | 公式 |
|---|---|---|---|
| `gps.lat/lon`、`global_position.lat/lon`、`drone_id.location.latitude/longitude`、`drone_id.system.operator_latitude/longitude` | degE7 (int32) | 度 (double) | `/1e7` |
| `gps.alt`、`global_position.alt` | mm (int32) | 米 (double) | `/1000.0` |
| `gps.alt_ellipsoid`、`gps.h_acc`、`gps.v_acc` | mm (int32/uint32) | 米 (double) | `/1000.0`（同 `gps.alt` 一样是 mm 刻度） |
| `global_position.hdg` | cdeg | 度 (double) | `/100.0` |
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
| `drone_id.location.altitude_barometric/altitude_geodetic`、`drone_id.system.operator_altitude_geo` | 已是米 (float) | 原样 | 无（哨兵值见下） |
| `remote_id.last_success_ms` | ms（开机时刻） | 原样 | 无（没有更合适的单位） |

`gps.eph`/`gps.epv`（GPS HDOP/VDOP，无量纲×100 的数值，不是物理量）**不换算，保持原始整数**，跟其它标量字段不同——按官方注释这两个是"unitless * 100"，除以100后也谈不上"物理单位"，直接透传更符合"存原样、不换算无意义数字"的原则；哨兵规则见下。

**哨兵值 → `null`**：MAVLink 用特殊值表示"未提供"，换算前先判断是否命中哨兵值，命中则该字段输出 `null`（不换算出无意义的数字）：
- `gps.eph/epv` = `UINT16_MAX`
- `sys_status.current_battery`/`battery.current_battery`/`battery.battery_remaining` = `-1`
- `battery.voltages[]`/`voltages_ext[]` 里等于 `UINT16_MAX`（或 `voltages_ext` 里等于 `0`，按官方注释是"不支持"）的槽位
- `drone_id.location.altitude_barometric/altitude_geodetic`、`drone_id.system.operator_altitude_geo` 等于 `-1000.0f`

## 6. 自定义扩展字段的 JSON key 名对照

`telemetry.battery2/motor/gnss_sat/humidity/lora/remote_id` 来自 `state_store.hpp` 里的自定义 struct（不是官方 MAVLink 消息，字段名可以自己定），具体 key 名和含义：

- **`battery2`**（对应 `Battery2Status`）：
  - `voltage_v`：电压，伏特 (double)，原始字段 `voltage_mv`(mV) `/1000.0`
  - `percent`：电量百分比 (uint8)，原样透传
  - `low_voltage`：是否低电压告警 (bool)，原样透传
- **`motor`**（对应 `MotorPwm`）：
  - `duty_percent`：4 路电机占空比数组 `[m1,m2,m3,m4]`(uint8 0-100)，已经是百分比，原样透传
  - `run_state`：整机运行状态 (bool)，原样透传（`MOTOR12`/`MOTOR34` 两帧的冗余拷贝，取最新一帧的值）
  - `speed_level`：整机速度档位 (uint8)，已经是百分比，原样透传（同上，冗余拷贝取最新值）
- **`gnss_sat`**（对应 `GnssSat`）：`gps_visible`/`beidou_visible`/`gps_used`/`beidou_used`，均为可见/使用卫星数 (uint8)，原样透传，字段名跟 struct 一致
- **`humidity`**（对应 `EnvHumidity`）：
  - `humidity_percent`：相对湿度百分比 (double)，原始字段 `relative_humidity_x10` `/10.0`
- **`lora`**（对应 `LoraStatus`）：
  - `loss_rate_percent`：估算丢包率百分比 (double)，原始字段 `loss_rate_x10` `/10.0`
  - `node_id`：LoRa 节点 ID (uint8)，原样透传
  - `present`：LoRa 模块是否在位 (bool)，原样透传
  - `link_state`：LoRa 链路状态字符串（第4节的模块状态枚举字符串表）
- **`remote_id`**（对应 `RemoteIdStatus`）：
  - `location_count`/`error_count`：位置广播成功/编码错误计数（增量语义，不是绝对值），原样透传
  - `last_success_ms`：RemoteID 最近一次成功提交时间（STM32 开机毫秒数），原样透传，没有更合适的单位可换算

## 7. `drone_id`/`alarms`/`logs` 细节

- `drone_id.basic_id/location/system/operator_id/self_id`：按第4、5节规则做枚举保留/单位换算。`uas_id`/`operator_id`/`description` 这几个 `char[]` 字段转成去除尾部空字符的字符串；`id_or_mac`（我们自己广播时恒为全零，字段本身是"仅用于接收其他飞行器数据时"）转成十六进制字符串——20字节对应40个十六进制字符，全零时为40个`'0'`组成的字符串。
- `drone_id.location`/`drone_id.system` 各自省略了几个官方消息字段（`speed_horizontal`/`speed_vertical`/`direction`/`height`/`height_reference`/`speed_accuracy`、`area_ceiling`/`area_floor`/`area_count`/`area_radius`），理由和完整清单见第1节——这个箱子固定不动、不编队，这些字段永远是没有意义的默认值。
- `alarms` = `{"ver": ..., "entries": [{"source_id","fault_code","severity","active","age_s"}, ...]}`，按 `active_count` 截断（`fault_code`/`source_id`/`severity` 业务含义未知，文档已注明，保持原始数字）。
- `logs` = `{"latest_seq": ..., "entries": [{"sequence","message_id","time":"HH:MM:SS","severity"}, ...]}`，按 `count` 截断；`time` 由 `time_hhmmss` 三元组拼成 `"HH:MM:SS"` 字符串。

## 8. main.cpp 集成

新增 `LogJsonPayload(const state::TelemetryState& state, const std::string& school_name)`，跟现有 `LogTelemetry`/`LogExtension` 同一个调用点（每处理完一帧调用一次），内部调用 `payload::ToJson(...).dump(2)` 打印到控制台，方便真机演示时人工核对。

## 9. 测试范围

`tests/test_json_serializer.cpp`：
- 空 `TelemetryState` → 输出只剩 `identity.school_name`，其余顶层 key 全部不存在。
- 每组字段（`telemetry.*`/`modules`/`alarms`/`logs`/`drone_id.*`）独立填充后，对应 key 存在且数值正确；未填充的字段仍然不存在。
- 单位换算抽样核对：`gps.lat/lon`、`attitude.roll`（弧度转角度）、`pressure.temperature`、`sys_status.voltage_battery`。
- 哨兵值 → `null`：至少覆盖 `gps.eph=UINT16_MAX`、`sys_status.current_battery=-1`、`drone_id.location.altitude_barometric=-1000.0f` 三种情况。
- 模块状态：14 项都输出正确的 `name`/`status` 字符串，含未收到过状态的模块（值为0，应显示 `UNINITIALIZED`，这是零初始化后的合法语义，不是异常）。
- `alarms.entries`/`logs.entries` 数组按 `active_count`/`count` 截断，含 0 条、中间值、满 14/9 条三种边界。
- `uas_id`/`operator_id`/`description` 去除尾部空字符后的字符串正确；`id_or_mac` 十六进制字符串格式正确。

## 10. 全局约束（供实施计划引用）

- `state_store` 层"存原样不做单位换算"的既定原则不变——所有单位换算只发生在 `payload/json_serializer` 这一层。
- 标准 MAVLink/ODID 官方枚举保持原始数字，不维护枚举名映射表；仅本项目自定义的模块状态枚举（含 `LORASTAT.link_state` 复用的同一枚举）转字符串。
- 未收到过的字段省略 JSON key，不输出 `null`；`null` 只用于"消息收到过，但该字段命中协议定义的哨兵值(表示未知/不支持)"这一种情况。
- key 一律 snake_case。
- 每个任务完成后运行对应测试，全绿才算完成；任务结束提交一次。
- 这套实训箱固定不动、不飞、不编队：`global_position.vx/vy/vz/relative_alt`、`gps.vel/cog/yaw/vel_acc/hdg_acc`、`drone_id.location.speed_horizontal/speed_vertical/direction/height/height_reference/speed_accuracy`、`drone_id.system.area_ceiling/area_floor/area_count/area_radius` 这些字段是无意义的飞行运动学量，**JSON里不输出**（`state_store`解码层不受影响，仍然原样存储官方结构体全部字段，只是`json_serializer`转换时跳过这些字段）；`attitude`（姿态角/角速度）保留，因为来自箱体上真实的MPU6050传感器。

## 11. 完整示例（全部字段有值，附逐字段含义）

真机上不会所有字段同时有值（`alarms` 没告警就不存在、`drone_id.*` 要等身份帧到达才有），下面这份是为了让每个字段的含义一次性过一遍，假装全部消息都收到过。用 `jsonc`（带注释）格式，注释是给人看的，实际序列化输出是纯 JSON、不含注释。

```jsonc
{
  "identity": {
    "vendor_id": "DCDWCNS1AB12CD34EF56",  // 厂商唯一产品识别码：全局唯一设备键，GB/T 41300结构(DCDWCNS1固定8位厂商代码 + 12位SN)
    "dcdw_label": "DCDW-001",              // 学校内部角色号：仅校内唯一，来自MAVLink帧头sysid，不能跨校当全局键用
    "rpi_serial": "10000000abcdef12",      // RPi本机硬件序列号(/proc/cpuinfo)：V1过渡期权威键，跟MAVLink帧无关
    "school_name": "NNUTC"                 // 学校名称：本机静态配置(config.json)，不是STM32解码出来的
  },
  "telemetry": {
    "heartbeat": {
      "custom_mode": 0,        // 自动驾驶仪自定义模式位域，含义由固件自己定义
      "type": 2,               // 飞行器/组件类型(MAV_TYPE，官方枚举，保持原始数字)
      "autopilot": 12,         // 自动驾驶仪类型(MAV_AUTOPILOT，官方枚举，保持原始数字)
      "base_mode": 81,         // 系统模式位图(官方定义，保持原始数字)
      "system_status": 4,      // 系统状态(MAV_STATE，官方枚举，保持原始数字)
      "mavlink_version": 3    // MAVLink协议版本号
    },
    "gps": {
      "time_usec": 1720000000000000,  // 时间戳(微秒)，UNIX纪元或开机以来，具体看数值量级
      "lat": 39.9042,                  // 纬度(度，WGS84)
      "lon": 116.4074,                 // 经度(度，WGS84)
      "alt": 43.5,                     // 海拔高度(米，MSL平均海平面基准)
      "alt_ellipsoid": 21.2,           // 椭球高度(米，WGS84椭球基准，跟alt的基准面不同)
      "eph": 120,                      // 水平精度衰减因子HDOP(无量纲×100，不换算，越小定位越准)
      "epv": 150,                      // 垂直精度衰减因子VDOP(无量纲×100，不换算)
      "fix_type": 3,                   // GPS定位状态(官方枚举，如3=3D定位，保持原始数字)
      "satellites_visible": 14,        // 可见卫星数
      "h_acc": 1.1,                    // 水平位置不确定度(米)
      "v_acc": 1.8                     // 垂直位置不确定度(米)
      // vel/cog/yaw/vel_acc/hdg_acc(地速/航迹方向/GPS偏航角及其不确定度)不输出：箱子固定不动，见第1节
    },
    "gnss_sat": {
      "gps_visible": 9,      // GPS可见卫星数
      "beidou_visible": 8,   // 北斗可见卫星数
      "gps_used": 7,         // GPS参与定位解算的卫星数
      "beidou_used": 6       // 北斗参与定位解算的卫星数
    },
    "attitude": {
      "time_boot_ms": 123456,  // 开机以来的时间戳(毫秒)
      "roll": 1.2,              // 横滚角(度，-180~180)
      "pitch": -0.8,            // 俯仰角(度)
      "yaw": 45.0,              // 偏航角(度)
      "rollspeed": 0.5,         // 横滚角速度(度/秒)
      "pitchspeed": -0.3,       // 俯仰角速度(度/秒)
      "yawspeed": 0.1           // 偏航角速度(度/秒)
    },
    "global_position": {
      "time_boot_ms": 123456,  // 开机以来的时间戳(毫秒)
      "lat": 39.9042,           // 纬度(度)
      "lon": 116.4074,          // 经度(度)
      "alt": 43.5,              // 海拔高度(米，MSL)
      "hdg": 87.5               // 机头朝向(度)
      // vx/vy/vz(速度分量)、relative_alt(相对起飞点高度)不输出：箱子固定不动、没有"起飞"，见第1节
    },
    "sys_status": {
      "onboard_control_sensors_present": 1483,           // 机载传感器/控制器"存在"位图(官方定义，每一位代表一种传感器，保持原始数字)
      "onboard_control_sensors_enabled": 1483,           // 同上，"启用"位图
      "onboard_control_sensors_health": 1483,            // 同上，"健康"位图
      "load": 23.5,                                       // 主循环CPU占用率(百分比)
      "voltage_battery": 12.6,                            // 电池电压(伏特)
      "current_battery": 3.25,                            // 电池电流(安培)
      "drop_rate_comm": 0.1,                              // 通信丢包率(百分比)
      "errors_comm": 0,                                   // 通信错误计数
      "errors_count1": 0,                                 // 自动驾驶仪自定义错误计数1(含义由固件自己定义)
      "errors_count2": 0,                                 // 自定义错误计数2
      "errors_count3": 0,                                 // 自定义错误计数3
      "errors_count4": 0,                                 // 自定义错误计数4
      "battery_remaining": 78,                            // 剩余电量(百分比)
      "onboard_control_sensors_present_extended": 0,      // 扩展传感器"存在"位图(用于超过32个传感器的场景)
      "onboard_control_sensors_enabled_extended": 0,      // 扩展"启用"位图
      "onboard_control_sensors_health_extended": 0        // 扩展"健康"位图
    },
    "battery": {
      "current_consumed": 1520,                                              // 已消耗电荷(毫安时)
      "energy_consumed": 18500.0,                                            // 已消耗能量(焦耳)
      "temperature": 28.5,                                                   // 电池温度(摄氏度)
      "voltages": [4.15, 4.14, 4.15, 4.13, null, null, null, null, null, null], // 1-10节电芯电压(伏特，未使用槽位为null)
      "current_battery": 3.25,                                               // 电池电流(安培)
      "id": 0,                                                                // 电池编号
      "battery_function": 1,                                                 // 电池用途类型(官方枚举，保持原始数字)
      "type": 1,                                                              // 电池化学类型(官方枚举，保持原始数字)
      "battery_remaining": 78,                                               // 剩余电量(百分比)
      "time_remaining": 3600,                                                // 预计剩余可用时间(秒)
      "charge_state": 2,                                                     // 电量状态告警等级(官方枚举，保持原始数字)
      "voltages_ext": [null, null, null, null],                              // 11-14节电芯电压(伏特，未使用为null)
      "mode": 0,                                                              // 电池模式(官方枚举，保持原始数字)
      "fault_bitmask": 0                                                     // 故障/健康位图(官方定义，保持原始数字)
    },
    "battery2": {
      "voltage_v": 12.6,       // 电压(伏特)——第二路电池，state_store里的Battery2Status
      "percent": 82,           // 电量百分比
      "low_voltage": false     // 是否触发低电压告警
    },
    "pressure": {
      "time_boot_ms": 123456,       // 开机以来的时间戳(毫秒)
      "press_abs": 1013.25,         // 绝对气压(百帕)
      "press_diff": 0.02,           // 差压(百帕，如空速管测量值)
      "temperature": 26.5,          // 气压计温度(摄氏度)
      "temperature_press_diff": 26.5 // 差压传感器温度(摄氏度)
    },
    "humidity": {
      "humidity_percent": 53.5   // 相对湿度(百分比)
    },
    "motor": {
      "duty_percent": [45, 45, 50, 50],  // 4路电机占空比(百分比数组，顺序对应电机1-4)
      "run_state": true,                  // 整机运行状态(true=运行中；MOTOR12/MOTOR34两帧的冗余拷贝，取最新一帧)
      "speed_level": 60                   // 整机速度档位(百分比，同上取最新一帧)
    },
    "lora": {
      "loss_rate_percent": 1.5,  // 估算丢包率(百分比)
      "node_id": 9,               // LoRa节点编号
      "present": true,            // LoRa模块是否在位
      "link_state": "ONLINE"      // LoRa链路状态(字符串；跟modules里LORA模块状态语义相同但独立更新，不做一致性校验)
    },
    "remote_id": {
      "location_count": 120,      // 位置广播成功计数(增量语义，不是累计总数)
      "error_count": 0,            // 编码/提交错误计数(增量语义)
      "last_success_ms": 987654    // RemoteID最近一次成功提交时间(STM32开机毫秒数)
    }
  },
  "modules": [
    // 14个模块的健康状态；status取值:UNINITIALIZED/STARTING/ONLINE/DEGRADED/OFFLINE/FAILED/DISABLED
    { "name": "GNSS", "status": "ONLINE" },
    { "name": "IMU", "status": "ONLINE" },
    { "name": "BARO", "status": "ONLINE" },
    { "name": "BATTERY", "status": "ONLINE" },
    { "name": "LORA", "status": "DEGRADED" },
    { "name": "5G", "status": "OFFLINE" },
    { "name": "STORAGE", "status": "ONLINE" },
    { "name": "REMOTE_ID", "status": "ONLINE" },
    { "name": "DISPLAY", "status": "DISABLED" },
    { "name": "CONTROL", "status": "ONLINE" },
    { "name": "ALARM", "status": "ONLINE" },
    { "name": "SYSTEM", "status": "ONLINE" },
    { "name": "ESTIMATOR", "status": "STARTING" },
    { "name": "BUSINESS", "status": "ONLINE" }
  ],
  "alarms": {
    "ver": 1,   // 告警表版本号
    "entries": [
      // source_id=告警来源模块编号；fault_code=故障代码(业务含义待固件侧文档，RPi只透传)；
      // severity=严重等级(原始数字)；active=是否仍然有效；age_s=告警已持续时间(秒)
      { "source_id": 4, "fault_code": 1032, "severity": 2, "active": true, "age_s": 15 },
      { "source_id": 9, "fault_code": 2004, "severity": 1, "active": false, "age_s": 320 }
    ]
  },
  "logs": {
    "latest_seq": 458,  // 最新日志序号
    "entries": [
      // sequence=日志序号；message_id=日志消息ID(含义待固件侧文档)；time=HH:MM:SS；severity=严重等级(原始数字)
      { "sequence": 456, "message_id": 12, "time": "14:23:07", "severity": 1 },
      { "sequence": 457, "message_id": 45, "time": "14:23:09", "severity": 2 },
      { "sequence": 458, "message_id": 3, "time": "14:23:11", "severity": 0 }
    ]
  },
  "drone_id": {
    "basic_id": {
      "target_system": 0,       // 目标系统ID(0=广播)
      "target_component": 0,    // 目标组件ID(0=广播)
      "id_or_mac": "0000000000000000000000000000000000000000",  // 仅用于接收其他飞行器数据时的MAC/ID；我们自己广播时恒为全零
      "id_type": 1,              // UAS ID格式类型(官方枚举，保持原始数字)
      "ua_type": 2,              // 无人机类型(官方枚举，保持原始数字)
      "uas_id": "DCDWCNS1AB12CD34EF56"  // 无人机唯一识别码，即厂商唯一产品识别码，同identity.vendor_id
    },
    "location": {
      "latitude": 39.9042,           // 无人机当前纬度(度)
      "longitude": 116.4074,         // 无人机当前经度(度)
      "altitude_barometric": 45.2,   // 气压高度(米)
      "altitude_geodetic": 44.8,     // WGS84大地高度(米)
      "timestamp": 1234.5,           // UTC整点后的秒数
      "target_system": 0,
      "target_component": 0,
      "id_or_mac": "0000000000000000000000000000000000000000",
      "status": 2,                    // 飞行器地面/空中状态(官方枚举，保持原始数字)
      "horizontal_accuracy": 4,       // 水平位置精度等级(官方枚举，保持原始数字)
      "vertical_accuracy": 4,         // 垂直位置精度等级(官方枚举，保持原始数字)
      "barometer_accuracy": 3,        // 气压高度精度等级(官方枚举，保持原始数字)
      "timestamp_accuracy": 2         // 时间戳精度等级(官方枚举，保持原始数字)
      // speed_horizontal/speed_vertical/direction/height/height_reference/speed_accuracy
      // 不输出：箱子固定不动，速度/相对高度类字段没有意义，见第1节
    },
    "system": {
      "operator_latitude": 39.905,    // 操作员纬度(度)
      "operator_longitude": 116.408,  // 操作员经度(度)
      "operator_altitude_geo": 45.0,  // 操作员大地高度(米)
      "timestamp": 233366400,         // UNIX时间戳(秒，2019-01-01 00:00:00起)
      "target_system": 0,
      "target_component": 0,
      "id_or_mac": "0000000000000000000000000000000000000000",
      "operator_location_type": 0,    // 操作员位置类型(官方枚举，保持原始数字)
      "classification_type": 0,       // 无人机分类类型(官方枚举，保持原始数字)
      "category_eu": 0,               // 欧盟分类下的类别(官方枚举，保持原始数字)
      "class_eu": 0                   // 欧盟分类下的级别(官方枚举，保持原始数字)
      // area_ceiling/area_floor/area_count/area_radius 不输出：编队/多机场景专用字段，这个箱子是单机，见第1节
    },
    "operator_id": {
      "target_system": 0,
      "target_component": 0,
      "id_or_mac": "0000000000000000000000000000000000000000",
      "operator_id_type": 0,           // operator_id字段的格式类型(官方枚举，保持原始数字)
      "operator_id": "CAAB1234567890"  // 操作员执照编号等文本(去除尾部空字符)
    },
    "self_id": {
      "target_system": 0,
      "target_component": 0,
      "id_or_mac": "0000000000000000000000000000000000000000",
      "description_type": 0,               // description字段的格式类型(官方枚举，保持原始数字)
      "description": "CNS-RPI training kit" // 自由文本描述(去除尾部空字符)
    }
  }
}
```
