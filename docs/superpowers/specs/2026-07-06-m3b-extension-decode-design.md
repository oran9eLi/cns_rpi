# M3b 扩展帧解码 —— 设计文档

日期：2026-07-06
状态：待用户复核

## 1. 背景与范围

`docs/V1设计文档.md` §10 已经把 M3b 的范围定死：`NAMED_VALUE_INT`（`MODSTAT0`/`MODSTAT1`/`BAT2STAT`/`MOTORPWM`/`GNSS_SAT`/`ENVHUM`）+ `TUNNEL`（告警表 `payload_type=0x8001`、日志增量 `payload_type=0x8002`）的语义解析，写入 `state_store`。字段字典见同文档 §4.1/§4.2，均已从固件源码取得，不是猜测。

**不在本次范围内**：
- `LORASUM`——文档已注明"跟 RPi 关系不大，可以不接"，不实现。
- `OPEN_DRONE_ID_*` 身份帧——留给 M3c。
- 任何单位换算（mV→V、湿度×10→百分比等）——留给 M4 `payload/json_serializer`。

## 2. 核心设计决策

### 2.1 拆包但不换算

`NAMED_VALUE_INT` 的 `value` 是一个按位打包的 `int32_t`，不拆包的话这个字段对下游（M4 JSON 序列化）完全不可读——所以拆包成有名字的子字段是 M3b 必须做的结构提取，不是可选项。

但拆包之后，子字段**保持 wire 上的原始整数刻度**，不做单位换算：
- `ENVHUM` 存 `535`（不存 `53.5`）
- `BAT2STAT` 的电压存 `12600`（mV，不存 `12.6` V）

这样跟 M3a"存官方 struct、不做单位换算"的原则保持一致，换算逻辑统一留给 M4。

### 2.2 MODSTAT0/MODSTAT1 合并存储

`MODSTAT0`（模块 0-7）和 `MODSTAT1`（模块 8-13）是两条独立的 `NAMED_VALUE_INT` 帧，但语义上是同一份"14 个模块状态"数据的两半。`state_store` 把它们合并成一个 14 元素数组，每条帧到达时只覆写自己负责的那一半，不动另一半——这样对下游来说这本来就是一份数据，不需要 consumer 侧再拼一次。

这跟 M3a"整字段覆盖"的更新模式不同（M3a 每个 `Update*` 覆盖整个字段），因此 `UpdateModStatusLow`/`UpdateModStatusHigh` 是两个新的、只写数组一部分的方法。

模块状态本身存原始 `uint8_t`（取值 0-6，含义见 V1设计文档§4.1"模块状态枚举"表），不新建枚举类型包装——跟 M3a 里官方 struct 字段"数值+注释文档"的风格保持一致，不引入额外的项目自定义类型。

### 2.3 TUNNEL 容器与边界校验

两种 payload 都是变长表格，内部自带计数字段（`active_count`/`count`），但协议本身有硬上限（14 行/9 条），且实际可读字节数受 `payload_length`（最大 128 字节）约束。

存储用固定大小 `std::array` + 一个"实际有效行数"字段：

```cpp
struct AlarmEntry {
  std::uint8_t source_id;
  std::uint16_t fault_code;
  std::uint8_t severity;
  bool active;
  std::uint16_t age_s;
};
struct AlarmTable {
  std::uint8_t ver;
  std::array<AlarmEntry, 14> entries;
  std::size_t active_count;  // clamp 后的有效行数，<=14
};

struct LogEntry {
  std::uint16_t sequence;
  std::uint16_t message_id;
  std::array<std::uint8_t, 3> time_hhmmss;
  std::uint8_t severity;
};
struct MessageLog {
  std::uint16_t latest_seq;
  std::array<LogEntry, 9> entries;
  std::size_t count;  // clamp 后的有效条数，<=9
};
```

解析时把帧里声明的计数 clamp 到两个上限的较小值：
1. 协议硬上限（14 / 9）
2. `payload_length` 实际能容纳的完整行数：`(payload_length - 表头字节数) / 单行字节数`

如果 `payload_length` 连表头都不够（< 2 字节 / < 3 字节），整帧视为畸形，`DecodeExtensionAndStore` 返回 `false`、不写入 store——和"不认识的消息类型"走同一种"安静失败"语义，不引入新的错误建模方式（不用 `std::expected`，跟 Global Constraints 保持一致）。

`count=0`（日志增量 payload_type=0x8002）是合法值，表示"只有心跳，只有 `latest_seq` 有意义"，不当畸形处理。

### 2.4 文件组织与函数命名

沿用 `docs/V1设计文档.md` §8 目录设计里已经写好的 `src/protocol/extension_decoder.hpp/.cpp`，本次只实现 `NAMED_VALUE_INT`/`TUNNEL` 部分；`OPEN_DRONE_ID_*` 留给 M3c 往同一个文件里加，不新起文件、不用现在决定 M3c 要不要拆出去。

**命名冲突提醒**：`telemetry_decoder.hpp` 已经在 `protocol` 命名空间下定义了 `bool DecodeAndStore(const mavlink_message_t&, state::StateStore&)`。`extension_decoder.hpp` **不能**用同名同签名的自由函数（会导致重复定义/链接错误），本次用 `bool protocol::DecodeExtensionAndStore(const mavlink_message_t&, state::StateStore&)` 加以区分。

### 2.5 `main.cpp` 集成

每条收到的消息依次尝试两个解码器（各自 `switch`/`default: return false`，互不影响，开销可忽略）：

```cpp
if (auto msg = link->ReceiveMessage()) {
  if (protocol::DecodeAndStore(*msg, state_store)) {
    LogTelemetry(msg->msgid, state_store.Snapshot());
  } else if (protocol::DecodeExtensionAndStore(*msg, state_store)) {
    LogExtension(msg->msgid, state_store.Snapshot());
  }
}
```

`LogExtension` 是新增的匿名命名空间辅助函数，风格上和现有 `LogTelemetry` 一致，供真机验证时人工核对字段用。

## 3. `state_store` 新增字段

```cpp
struct TelemetryState {
  // ...既有 M3a 字段不变...
  std::optional<std::array<std::uint8_t, 14>> module_status;
  std::optional<Battery2Status> battery2_status;
  std::optional<MotorPwm> motor_pwm;
  std::optional<GnssSat> gnss_sat;
  std::optional<EnvHumidity> env_humidity;
  std::optional<AlarmTable> alarm_table;
  std::optional<MessageLog> message_log;
};
```

其中：

```cpp
struct Battery2Status {
  std::uint16_t voltage_mv;
  std::uint8_t percent;
  bool low_voltage;
};
struct MotorPwm {
  std::array<std::uint8_t, 4> duty_percent;
};
struct GnssSat {
  std::uint8_t gps_visible;
  std::uint8_t beidou_visible;
  std::uint8_t gps_used;
  std::uint8_t beidou_used;
};
struct EnvHumidity {
  std::uint16_t relative_humidity_x10;
};
```

`StateStore` 新增方法：

```cpp
void UpdateModStatusLow(const std::array<std::uint8_t, 8>& modules0to7);
void UpdateModStatusHigh(const std::array<std::uint8_t, 6>& modules8to13);
void UpdateBattery2Status(const Battery2Status&);
void UpdateMotorPwm(const MotorPwm&);
void UpdateGnssSat(const GnssSat&);
void UpdateEnvHumidity(const EnvHumidity&);
void UpdateAlarmTable(const AlarmTable&);
void UpdateMessageLog(const MessageLog&);
```

`UpdateModStatusLow`/`UpdateModStatusHigh`：若 `module_status` 尚无值，先零初始化整个 14 元素数组，再写入自己负责的那一半；若已有值，只覆写自己负责的 8 个/6 个元素，另一半保留原值。其余 `Update*` 与 M3a 风格一致：加锁、整体赋值。

## 4. `protocol::DecodeExtensionAndStore` 解码逻辑

```cpp
bool DecodeExtensionAndStore(const mavlink_message_t& msg, state::StateStore& store);
```

- `msg.msgid == MAVLINK_MSG_ID_NAMED_VALUE_INT`：`mavlink_msg_named_value_int_decode` 拿到 `mavlink_named_value_int_t`，按 `name` 字段（`char[10]`，**不保证有 `\0`**，用 `std::string_view(value.name, strnlen(value.name, 10))` 安全取值再比较）二次分派到 6 个具体名字；不认识的名字返回 `false`。
- `msg.msgid == MAVLINK_MSG_ID_TUNNEL`：`mavlink_msg_tunnel_decode` 拿到 `mavlink_tunnel_t`，按 `payload_type` 二次分派到告警表/日志增量两种解析；不认识的 `payload_type` 返回 `false`。
- 其余 `msgid` 一律返回 `false`（保持和 `telemetry_decoder` 同样的"安静忽略"语义）。

### 4.1 `NAMED_VALUE_INT` 位打包解码（对应 V1设计文档§4.1）

先 `auto bits = static_cast<std::uint32_t>(value.value);` 避免对有符号数做位移的未定义行为，再按名字解析：

| name | 解码 |
|---|---|
| `MODSTAT0` | `for i in 0..7: module[i] = (bits >> (i*4)) & 0xF` → `UpdateModStatusLow` |
| `MODSTAT1` | `for i in 0..5: module[8+i] = (bits >> (i*4)) & 0xF` → `UpdateModStatusHigh` |
| `BAT2STAT` | `voltage_mv = bits & 0xFFFF`；`percent = (bits>>16) & 0xFF`；`low_voltage = (bits>>24) & 0x1` |
| `MOTORPWM` | `for i in 0..3: duty_percent[i] = (bits >> (i*8)) & 0xFF` |
| `GNSS_SAT` | `gps_visible=bits&0xFF`；`beidou_visible=(bits>>8)&0xFF`；`gps_used=(bits>>16)&0xFF`；`beidou_used=(bits>>24)&0xFF` |
| `ENVHUM` | `relative_humidity_x10 = static_cast<std::uint16_t>(bits)`（文档取值范围 0-1000，直接截断即可） |

### 4.2 `TUNNEL` 字节级解码（对应 V1设计文档§4.2）

payload 是裸字节数组（`uint8_t payload[128]`），没有官方 struct 字段映射，需要手写小端读取辅助函数（放在 `.cpp` 匿名命名空间，不进头文件）：`ReadU16LE(payload, offset)`。

**`payload_type=0x8001`（告警表）**：
- 表头 2 字节：`ver`(1) + `active_count`(1)
- 若 `payload_length < 2` → 畸形，返回 `false`
- `usable_rows = min(active_count, 14, (payload_length - 2) / 7)`
- 每行 7 字节，从 offset 2 开始：`source_id`(1) + `fault_code`(2,LE) + `severity`(1) + `active`(1，非0即真) + `age_s`(2,LE)

**`payload_type=0x8002`（日志增量）**：
- 表头 3 字节：`latest_seq`(2,LE) + `count`(1)
- 若 `payload_length < 3` → 畸形，返回 `false`
- `usable_entries = min(count, 9, (payload_length - 3) / 8)`
- 每条 8 字节，从 offset 3 开始：`sequence`(2,LE) + `message_id`(2,LE) + `time_hhmmss`(3 原始字节) + `severity`(1)
- `count=0` 是合法值（只有心跳），不是畸形

## 5. 测试范围

`tests/test_extension_decoder.cpp`（沿用 `test_telemetry_decoder.cpp` 的 doctest 风格）：

1. 6 个 `NAMED_VALUE_INT` 名字各一个用例，核对拆包后的子字段数值（含 `MODSTAT0`+`MODSTAT1` 分两次调用后 14 元素数组都符合预期、互不覆盖的用例）
2. `TUNNEL` 告警表一个用例（含 1-2 行数据）
3. `TUNNEL` 日志增量一个用例（含 1-2 条数据，及 `count=0` 只有心跳的用例）
4. 边界用例：
   - 不认识的 `name` 被安静忽略（`handled==false`），不影响其他已有字段
   - 不认识的 `payload_type` 被安静忽略
   - 告警表/日志增量的声明计数超过协议上限（14/9）时被 clamp
   - `payload_length` 不足以容纳声明的行数时被 clamp 到实际能容纳的行数
   - `payload_length` 小于表头长度时整帧判定畸形，返回 `false`、不写入 store

`tests/test_state_store.cpp` 补充：`UpdateModStatusLow`/`UpdateModStatusHigh` 各自只影响自己负责的那一半，互不覆盖。

## 6. CMakeLists.txt 改动

- `cns_rpi_core` 源文件列表新增 `src/protocol/extension_decoder.cpp`
- 新增 `test_extension_decoder` 可执行文件 + `add_test`

## 7. 文档同步

`docs/V1设计文档.md` §10 M3b 一行落地后标记已实现（沿用 M3a 完成时的写法）。§4.1/§4.2 字段字典本身不需要改动（已经是解码依据，验证过没有出入）。
