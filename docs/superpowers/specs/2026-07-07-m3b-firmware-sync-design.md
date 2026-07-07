# M3b 字段字典跟固件同步 + 新增 RPi 专属遥测 设计文档

版本：2026-07-07
状态：已批准，待写实施计划
适用范围：`docs/V1设计文档.md` §4.1 字段字典的订正，不是新里程碑——固件侧 `Formal_Framework` PR#1（`feat/fj-lora-fusion`）落地后，RPi 侧 `NAMED_VALUE_INT` 解码需要跟着同步。

## 1. 背景

固件侧这次 PR 把 `docs/固件与RPi改动分工.md`/`docs/RPi侧解码对齐清单.md` 里 D1/D2 决策落地并额外新增了 2 条 RPi 专属遥测。已用真机源码（`px4lite_mavlink_tx.c`）逐字段核对过实际 wire 格式：

| 固件现状 | RPi 现状（M3b 已实现） | 差异 |
|---|---|---|
| `HUMIDITY`（`value`=湿度×10，不拆位） | 键的是 `ENVHUM` | 改名 |
| `MOTOR12`/`MOTOR34`（每帧2路电机+run_state+speed_level） | 等单条 `MOTORPWM`（4路一帧） | 换分支 |
| `LORASTAT`（LoRa链路状态，RPi专属） | 未解 | 新增 |
| `RIDSTAT`（RemoteID广播状态，RPi专属） | 未解 | 新增 |
| 告警表/日志/身份（TUNNEL 0x8001/0x8002、`OPEN_DRONE_ID_*`） | 已解（M3b/M3c） | 无需改，已核对逐字节一致 |

固件不会再发 `ENVHUM`/`MOTORPWM` 这两个旧 name 了，RPi 侧对应的旧分支是死代码，直接删除，不保留兼容分支（`docs/协作规则.md`"不留死代码"的既定做法）。

## 2. Wire 格式（已用固件源码核对，非猜测）

### 2.1 `HUMIDITY`（原 `ENVHUM`，布局不变）

`value`(int32) = 相对湿度×10，直接存整数，不拆位。`time_boot_ms` = 采样时刻。跟原 `ENVHUM` 分支逻辑完全一样，只改字符串比较。

### 2.2 `MOTOR12`/`MOTOR34`

每帧管 2 路电机：

| name | value 位布局 |
|---|---|
| `MOTOR12` | `[0:7]`电机1占空% `[8:15]`电机2占空% `[16]`run_state(0/1) `[24:31]`speed_level% |
| `MOTOR34` | `[0:7]`电机3占空% `[8:15]`电机4占空% `[16]`run_state `[24:31]`speed_level |

`run_state`/`speed_level` 两帧携带的是同一份整机状态的冗余拷贝，不需要合并逻辑——哪帧后到就用哪帧的值覆盖，不用像 `duty_percent` 那样区分"自己负责哪一半"。`duty_percent` 需要合并（MOTOR12 只写索引0/1，MOTOR34 只写索引2/3，互不覆盖），沿用 M3b `UpdateModStatusLow`/`UpdateModStatusHigh` 那套"lazy 零初始化+各写自己那一半"的模式。

### 2.3 `LORASTAT`（RPi 专属，新增）

| 段 | 含义 |
|---|---|
| `[0:15]` | 接收侧估算丢包率×10（0..1000） |
| `[16:23]` | LoRa 节点 ID |
| `[24]` | LoRa 模块在位标志 |
| `[25:27]` | LoRa 链路状态枚举（`Px4Lite_State_t`，跟 `module_status[4]` 语义相同但不合并存储，见第 3 节） |

`time_boot_ms` = 当前时刻（无特殊含义）。

### 2.4 `RIDSTAT`（RPi 专属，新增）

| 段 | 含义 |
|---|---|
| `[0:15]` | 位置广播成功计数低16位（增量语义，不是绝对值） |
| `[16:31]` | 编码/提交错误计数低16位（同上） |

`time_boot_ms` = RemoteID 最近一次成功提交时间，RPi 可据此判断广播是否还在推进。

## 3. 数据存储设计

### 3.1 `MotorPwm` 扩展

```cpp
struct MotorPwm {
  std::array<std::uint8_t, 4> duty_percent;
  bool run_state;
  std::uint8_t speed_level;
};
```

### 3.2 `LoraStatus`/`RemoteIdStatus`——各自独立的 optional 字段，不合并进 `module_status`

讨论过是否把这两个塞进现有的 `module_status`（`std::array<std::uint8_t, 14>`，MODSTAT0/1 写入的粗粒度状态位图）。结论：**装不进，也不该装**——`module_status` 每个模块只占 1 字节存 0-6 的粗粒度枚举，`LoraStatus`/`RemoteIdStatus` 是完全不同形状的数据（丢包率+node_id+在位标志 / 两个计数器+时间戳），把数组元素类型改大来兼容会让其余 12 个不需要扩展信息的模块白白浪费空间，也会牵连已上线的 `UpdateModStatusLow`/`UpdateModStatusHigh` 现有实现。

`LORASTAT.link_state` 字段跟 `module_status[4]`（LORA 模块的粗粒度状态）语义重复，但保留在 `LoraStatus` 里原样存储——不做"读 module_status 代替"的省略，因为 `state_store` 的既定原则是"存官方/固件发的数据原样，不做二次加工"，两个字段来自两条不同的帧，各自独立更新即可，冗余但简单，不需要额外的一致性维护逻辑。

最终采用跟 M3b 里 `GnssSat`/`Battery2Status` 完全一样的模式——各起一个具名 struct，`TelemetryState` 上各挂一个 `std::optional`：

```cpp
struct LoraStatus {
  std::uint16_t loss_rate_x10;
  std::uint8_t node_id;
  bool present;
  std::uint8_t link_state;
};

struct RemoteIdStatus {
  std::uint16_t location_count;
  std::uint16_t error_count;
  std::uint32_t last_success_ms;
};
```

`TelemetryState` 新增：
```cpp
std::optional<LoraStatus> lora_status;
std::optional<RemoteIdStatus> remote_id_status;
```

`StateStore` 新增 `UpdateLoraStatus`、`UpdateRemoteIdStatus`（整字段覆盖，不需要合并逻辑，模式同 `UpdateBattery2Status`/`UpdateGnssSat`）。

### 3.3 `MotorPwm` 的合并写入方法

```cpp
void UpdateMotorPwmLow(std::uint8_t duty0, std::uint8_t duty1, bool run_state, std::uint8_t speed_level);   // MOTOR12 -> duty_percent[0..1]
void UpdateMotorPwmHigh(std::uint8_t duty2, std::uint8_t duty3, bool run_state, std::uint8_t speed_level);  // MOTOR34 -> duty_percent[2..3]
```

两者都是：若 `motor_pwm` 之前没有值，先零初始化整个 struct；然后只写自己负责的 2 个 `duty_percent` 索引；`run_state`/`speed_level` 直接覆盖（两帧值相同，无需保留旧值）。原来整帧写 4 路的 `UpdateMotorPwm` 方法删除。

## 4. `protocol/extension_decoder.cpp` 改动

`DecodeNamedValueInt` 里：

- 删除 `if (name == "MOTORPWM") { ... }` 整个分支。
- `if (name == "ENVHUM")` 改成 `if (name == "HUMIDITY")`，分支内部逻辑不变。
- 新增 `if (name == "MOTOR12" || name == "MOTOR34")`：按上面的位布局解出 duty0/duty1/run_state/speed_level，调用 `UpdateMotorPwmLow`（MOTOR12）或 `UpdateMotorPwmHigh`（MOTOR34）。
- 新增 `if (name == "LORASTAT")`：解出 loss_rate_x10/node_id/present/link_state，调用 `UpdateLoraStatus`。
- 新增 `if (name == "RIDSTAT")`：解出 location_count/error_count，`time_boot_ms` 存入 `last_success_ms`，调用 `UpdateRemoteIdStatus`。

## 5. main.cpp

`LogExtension` 里：`HUMIDITY`/`MOTOR12`/`MOTOR34`/`LORASTAT`/`RIDSTAT` 五个新分支的打印（沿用现有风格，打印刚解出来的关键字段供真机人工验证）。

## 6. 测试范围

`tests/test_state_store.cpp`：
- 删除/更新 `MotorPwm` 相关的既有测试断言（字段变了）。
- 新增 `UpdateMotorPwmLow`/`UpdateMotorPwmHigh`/`UpdateLoraStatus`/`UpdateRemoteIdStatus` 的正向测试（不重复 M3c Task1 review 提过的"只测 nullopt 不测正向写入"的 Minor）。

`tests/test_extension_decoder.cpp`：
- 删除旧 `MOTORPWM` 解码测试。
- `ENVHUM` 测试改名成 `HUMIDITY`（断言逻辑不变）。
- 新增 `MOTOR12`/`MOTOR34` 解码测试，含"`MOTOR34` 不影响 `MOTOR12` 已写入的 duty_percent[0:1]"这种独立性测试（跟 M3b 的 `MODSTAT0`/`MODSTAT1` 一个套路）。
- 新增 `LORASTAT`/`RIDSTAT` 解码测试。
- 回归：现有 `GNSS_SAT`/`BAT2STAT`/`MODSTAT0`/`MODSTAT1`/TUNNEL/身份帧测试不改代码，跑一遍确认仍然全绿。

## 7. 文档同步

- `docs/V1设计文档.md` §4.1：`ENVHUM`→`HUMIDITY`，`MOTORPWM`→`MOTOR12`/`MOTOR34`，新增 `LORASTAT`/`RIDSTAT` 两行，加一句说明这是跟固件 PR 对齐后的订正，不是新里程碑。
- `docs/固件对接-数据格式.md` 第二部分：同步这几处字段的现状描述。
- `docs/MAVLink消息清单.md` 第2节：同步。

## 8. 全局约束（供实施计划引用）

- 官方/固件发的数据原样存储，不做单位换算、不做跨字段一致性校验（如 `LoraStatus.link_state` 和 `module_status[4]` 冗余但不互相校验）。
- 固件不再发送的旧 name（`ENVHUM`/`MOTORPWM`）对应的解码分支直接删除，不保留、不注释掉。
- `run_state`/`speed_level` 类"两帧冗余拷贝"的字段直接覆盖式更新，不需要合并逻辑；`duty_percent` 类"两帧分别负责一半"的字段需要 lazy 零初始化+各写各的一半（沿用 `UpdateModStatusLow`/`High` 模式）。
- 每个任务完成后运行对应测试，全绿才算完成；任务结束提交一次。
