# M3a：基础遥测解码 设计

日期：2026-07-03
状态：草案，待用户审阅
对应里程碑：`docs/V1设计文档.md` §10 M3a

## 1. 背景

M2 已经把 UART 字节流跑通到"一条通过 CRC 校验的完整 `mavlink_message_t` 帧"这一步（`uart::MavlinkLink::ReceiveMessage()`），但不解析帧内部字段——`main.cpp` 现在只打印 `msgid/len/sysid`。

M3a 的目标：把 M3a 范围内的 7 种标准 MAVLink 消息（`HEARTBEAT`/`GPS_RAW_INT`/`ATTITUDE`/`GLOBAL_POSITION_INT`/`SYS_STATUS`/`BATTERY_STATUS`/`SCALED_PRESSURE`）解码成具体字段，写入一份内部状态（`state_store`），符合 `docs/V1设计文档.md` §3 的解耦原则——解码层只管"把 STM32 发来的数据变成一份内部状态"，不关心谁来读；V1 唯一的消费者是 MQTT 发布（M5 才接），M3a 阶段先只把状态存好，`main.cpp` 打印出来做人工验证。

**范围边界**：这次只定 M3a 这 7 种标准消息。扩展帧（`NAMED_VALUE_INT`/`TUNNEL`，M3b）和身份帧（`OPEN_DRONE_ID_*`，M3c）不在这次设计范围内，不为它们预留占位字段——各自等实现那一步再单独走 brainstorming 设计，避免现在猜错形状。

## 2. `state/state_store` 设计

存官方 MAVLink 结构体原样，不做单位换算、不筛选字段——解码层只调官方 `mavlink_msg_*_decode()`，单位换算/字段筛选留给 M4 的 `payload/json_serializer` 去做，符合"M3 只管解码、M4 管 payload 构造"的里程碑边界。

```cpp
// state/state_store.hpp
struct TelemetryState {
  std::optional<mavlink_heartbeat_t> heartbeat;
  std::optional<mavlink_gps_raw_int_t> gps_raw_int;
  std::optional<mavlink_attitude_t> attitude;
  std::optional<mavlink_global_position_int_t> global_position_int;
  std::optional<mavlink_sys_status_t> sys_status;
  std::optional<mavlink_battery_status_t> battery_status;
  std::optional<mavlink_scaled_pressure_t> scaled_pressure;
};

class StateStore {
 public:
  void UpdateHeartbeat(const mavlink_heartbeat_t&);
  void UpdateGpsRawInt(const mavlink_gps_raw_int_t&);
  void UpdateAttitude(const mavlink_attitude_t&);
  void UpdateGlobalPositionInt(const mavlink_global_position_int_t&);
  void UpdateSysStatus(const mavlink_sys_status_t&);
  void UpdateBatteryStatus(const mavlink_battery_status_t&);
  void UpdateScaledPressure(const mavlink_scaled_pressure_t&);

  TelemetryState Snapshot() const;  // 加锁拷贝返回，不返回引用/指针

 private:
  mutable std::mutex mutex_;
  TelemetryState state_;
};
```

- 每个子字段用 `std::optional` 包裹：程序刚启动、某条消息（比如低频的 `BATTERY_STATUS`）还没收到过时是 `std::nullopt`，不会把默认构造的全 0 值当成真实数据读出去。
- 锁粒度：一把 `mutex_` 保护整个 `TelemetryState`，不按消息类型拆分成 7 把锁——数据量小、更新频率最高约 1Hz，细粒度锁是过度设计。`Update*()` 系列方法加锁写入对应字段；`Snapshot()` 加锁拷贝整个 struct 返回，调用方拿到的是独立副本，不持有内部状态的引用，读的时候不用关心锁。
- 现在就加锁：M5 接 MQTT 发布时会有独立线程（避免 MQTT 重连/发布阻塞 UART 读取），提前定好线程安全接口，届时不用改 `state_store` 对外形状。

**字段覆盖核对**（逐一核对官方结构体字段是 `docs/MAVLink消息清单.md` §1"关键字段"列表的超集，不会遗漏）：

| M3a 消息 | `TelemetryState` 字段 | 官方结构体 | 字段 |
|---|---|---|---|
| HEARTBEAT | `heartbeat` | `mavlink_heartbeat_t` | `custom_mode`, `type`, `autopilot`, `base_mode`, `system_status`, `mavlink_version` |
| GPS_RAW_INT | `gps_raw_int` | `mavlink_gps_raw_int_t` | `time_usec`, `lat`, `lon`, `alt`, `eph`, `epv`, `vel`, `cog`, `fix_type`, `satellites_visible`, `alt_ellipsoid`, `h_acc`, `v_acc`, `vel_acc`, `hdg_acc`, `yaw` |
| ATTITUDE | `attitude` | `mavlink_attitude_t` | `time_boot_ms`, `roll`, `pitch`, `yaw`, `rollspeed`, `pitchspeed`, `yawspeed` |
| GLOBAL_POSITION_INT | `global_position_int` | `mavlink_global_position_int_t`（来自 `standard` 方言，经 `common/mavlink.h` → `standard/standard.h` 传递可见） | `time_boot_ms`, `lat`, `lon`, `alt`, `relative_alt`, `vx`, `vy`, `vz`, `hdg` |
| SYS_STATUS | `sys_status` | `mavlink_sys_status_t` | `onboard_control_sensors_{present,enabled,health}`（含 `_extended` 三个位图）, `load`, `voltage_battery`, `current_battery`, `drop_rate_comm`, `errors_comm`, `errors_count1~4`, `battery_remaining` |
| BATTERY_STATUS | `battery_status` | `mavlink_battery_status_t` | `current_consumed`, `energy_consumed`, `temperature`, `voltages[10]`, `voltages_ext[4]`, `current_battery`, `id`, `battery_function`, `type`, `battery_remaining`, `time_remaining`, `charge_state`, `mode`, `fault_bitmask` |
| SCALED_PRESSURE | `scaled_pressure` | `mavlink_scaled_pressure_t` | `time_boot_ms`, `press_abs`, `press_diff`, `temperature`, `temperature_press_diff` |

## 3. `protocol/telemetry_decoder` 设计

统一入口函数，`main.cpp` 收到帧后只调这一个函数，自己不判断 `msgid`：

```cpp
// protocol/telemetry_decoder.hpp
namespace protocol {
/// 尝试把 msg 解码成 M3a 范围内的 7 种标准消息之一并写入 store。
/// @return 是本函数认识的消息类型（成功解码并写入）返回 true；不认识的 msgid 返回 false，安静忽略、不报错。
bool DecodeAndStore(const mavlink_message_t& msg, state::StateStore& store);
}
```

```cpp
// protocol/telemetry_decoder.cpp
bool DecodeAndStore(const mavlink_message_t& msg, state::StateStore& store) {
  switch (msg.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT: {
      mavlink_heartbeat_t decoded{};
      mavlink_msg_heartbeat_decode(&msg, &decoded);
      store.UpdateHeartbeat(decoded);
      return true;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: { /* 同上模式 */ return true; }
    case MAVLINK_MSG_ID_ATTITUDE: { /* 同上模式 */ return true; }
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: { /* 同上模式 */ return true; }
    case MAVLINK_MSG_ID_SYS_STATUS: { /* 同上模式 */ return true; }
    case MAVLINK_MSG_ID_BATTERY_STATUS: { /* 同上模式 */ return true; }
    case MAVLINK_MSG_ID_SCALED_PRESSURE: { /* 同上模式 */ return true; }
    default:
      return false;
  }
}
```

不认识的消息类型（比如 STM32 以后发的其他遥测、或者 M3b/M3c 范围内的扩展帧/身份帧）现在会被安静忽略——这是正确行为，不是遗漏：M3a 只负责这 7 种，其余的等各自里程碑接入。

## 4. `main.cpp` 改动

收到帧后调用 `protocol::DecodeAndStore(msg, store)`；把现在打印裸 `msgid/len/sysid` 的日志，换成解码成功后打印有意义的字段（比如 HEARTBEAT 的 `type`/`system_status`，GPS_RAW_INT 的 `lat`/`lon`/`fix_type` 等），方便真机验证时肉眼确认解码对不对。不认识的消息类型（`DecodeAndStore` 返回 `false`）不打印。周期发送 HEARTBEAT 的逻辑（M2 已验证）保持不变。

## 5. 测试

`tests/test_telemetry_decoder.cpp`（doctest，不依赖真实串口）：

- 7 个 `TEST_CASE`，每个用官方 `mavlink_msg_*_pack()` 现造一条对应消息的样例帧，喂给 `DecodeAndStore`，断言返回 `true`，再从 `StateStore::Snapshot()` 里取出对应字段，逐字段核对和打包时传入的值一致。
- 1 个边界 `TEST_CASE`：喂一条 M3a 不认识的消息（比如 `STATUSTEXT`），断言 `DecodeAndStore` 返回 `false`，且不影响 `StateStore` 里其他已经写入过的字段（验证"忽略"是真的安静忽略，不会误写或崩溃）。

## 6. 影响范围 / 后续同步

- 不改变 `docs/V1设计文档.md` §8 目录结构（`state/`、`protocol/` 已经在结构里预留），本次只是把 `state_store.hpp/.cpp`、`telemetry_decoder.hpp/.cpp` 两个具体文件从"预留位"变成"已实现"。
- M3a 完成后，`docs/V1设计文档.md` §10 里程碑列表对应条目标记为已实现（同一次提交同步，按 `docs/协作规则.md` §7）。
- M3b（扩展帧）、M3c（身份帧）的 state 结构如何摆放（新增独立的 `ExtensionState`/`IdentityState`，还是并入 `TelemetryState`），留到各自的 brainstorming 阶段单独决定，不受本次设计约束。
