# M3c 身份帧解码 设计文档

版本：2026-07-07
状态：已批准，待写实施计划
适用范围：`docs/V1设计文档.md` §10 "M3c 身份帧解码" 里程碑

## 1. 范围

- **OPEN_DRONE_ID_* 系列，全部 5 个消息**：`OPEN_DRONE_ID_BASIC_ID` / `LOCATION` / `SYSTEM` / `OPERATOR_ID` / `SELF_ID`。虽然对不能飞行的实训箱来说 LOCATION/SYSTEM 的经纬度/速度类字段大概率是固件代码里的"unknown"哨兵值，但为了跟固件端 RemoteID 报文结构完全对应、给 V2 真做飞行模拟时留好底子，仍然全部解码。
- **DCDW 角色号解析**：从 MAVLink 帧头的 `sysid` 字段格式化出 `"DCDW-XXX"`，不是某个消息 payload 里的字段。
- **RPi 序列号读取**：从 `/proc/cpuinfo` 读硬件序列号，V1 过渡期权威键（详见 `docs/设备标识符.md` §1）。

**明确不做的事**（避免范围蔓延到别的里程碑）：

- 不实现"身份就绪后再连 MQTT"的启动等待逻辑——`V1设计文档.md` §2/§9 明确把这个启动依赖记在 M5 头上，M3c 只管解码+存储。
- 不校验 `id_type`/`ua_type` 是否符合预期取值，也不校验 `vendor_id` 的字符集/前缀是否合规——延续"STM32 是权威，RPi 拿到即用、不重新计算/不校验"的原则（`设备标识符.md` §2.3）。
- LOCATION/SYSTEM 里的经纬度（degE7）、速度（cm/s）、时间戳等字段照 M3a 的原则存官方 struct 原样，不做单位换算，留给 M4 `payload/json_serializer`。

## 2. 消息归属与解码方式

OPEN_DRONE_ID_* 5 个消息都是有名字段的官方 struct（不是 NAMED_VALUE_INT/TUNNEL 那种打包裸字段），解码方式接近 M3a 的"存官方 struct 原样"，不需要 M3b 式的位运算拆包。

归属 `src/protocol/extension_decoder.hpp/.cpp`——目录结构（`V1设计文档.md` §8）里早就写明这个文件负责 `NAMED_VALUE_INT/TUNNEL/OPEN_DRONE_ID_* 解码 -> 写入 state_store`，M3c 是在已有的 `DecodeExtensionAndStore` 函数里给 `switch (msg.msgid)` 加 5 个新 case，不新增文件、不新增函数名。

`vendor_id` 提取（唯一需要"拆包"的字段）：`OPEN_DRONE_ID_BASIC_ID.uas_id` 是 `uint8_t[20]`，未用部分填 null——跟 M3b 里 `NAMED_VALUE_INT.name` 字段同样的坑，用 `strnlen` 求实际长度再转 `std::string`，避免对 20 字节数组过读（尤其是恰好写满 20 字节、没有 null 终止符的边界情况，必须有测试覆盖——这也是 M3b 遗留的同类测试缺口，顺手一起补）。提取函数放在 `identity.hpp` 里（`ExtractVendorId`），由 `extension_decoder.cpp` 解码 BASIC_ID 时调用。

## 3. 数据存储（`state/state_store.hpp`）

`TelemetryState` 新增字段，跟现有字段同一个扁平结构，不另起子 struct：

```cpp
std::optional<mavlink_open_drone_id_basic_id_t> open_drone_id_basic_id;
std::optional<mavlink_open_drone_id_location_t> open_drone_id_location;
std::optional<mavlink_open_drone_id_system_t> open_drone_id_system;
std::optional<mavlink_open_drone_id_operator_id_t> open_drone_id_operator_id;
std::optional<mavlink_open_drone_id_self_id_t> open_drone_id_self_id;

std::optional<std::string> vendor_id;    // 从 BASIC_ID.uas_id 提取
std::optional<std::string> dcdw_label;   // FormatDcdwLabel(msg.sysid) 结果
std::optional<std::string> rpi_serial;   // ReadRpiSerial() 结果
```

`StateStore` 新增对应的 8 个 `Update*` 方法（5 个 OPEN_DRONE_ID_* struct + vendor_id + dcdw_label + rpi_serial），模式和现有方法一致：整字段覆盖，不是 M3b 那种部分合并（这 8 个字段都不存在"一个值分两条帧发"的情况）。

## 4. `protocol/identity.hpp/.cpp`（新文件）

只负责两件跟 MAVLink 消息 payload 内容无关的事，外加一个 M3c 专属的字节数组转字符串工具：

```cpp
/// 从 MAVLink 帧头 sysid 格式化 DCDW 角色号，3 位数字补零
/// （sysid 是 uint8_t，最大 255，3 位够用）。
/// 依据：设备标识符.md §3，这个数字就是固件的 PX4LITE_UNIT_ID / system_id。
std::string FormatDcdwLabel(std::uint8_t sysid);

/// path 参数仅供单元测试注入 fixture 文件用（tests/test_identity.cpp），
/// 真机代码（main.cpp）从不显式传参，永远用默认值读真实 /proc/cpuinfo。
/// 【测试专用】M3c 真机验证通过后，评估是否删掉这个参数（连同依赖它的
/// fixture 测试一起去掉）或注释掉，不作为长期对外接口保留。
std::optional<std::string> ReadRpiSerial(const std::filesystem::path& path = "/proc/cpuinfo");

/// 从 uas_id（20 字节，未用部分填 null）提取厂商唯一产品识别码字符串，
/// 用 strnlen 避免对没有 null 终止符的情况过读。不做格式校验。
/// 参数类型对应 mavlink_open_drone_id_basic_id_t::uas_id 的原始字段类型
/// （uint8_t[20]，C 数组，不是 std::array），按引用传递保留长度信息，
/// 调用点直接传 value.uas_id 即可，不需要额外转换。
std::string ExtractVendorId(const std::uint8_t (&uas_id)[20]);
```

## 5. main.cpp 集成

- 启动时调用一次 `identity::ReadRpiSerial()`，有值就 `store.UpdateRpiSerial(*serial)`。
- 收帧循环里，不管 `DecodeAndStore`/`DecodeExtensionAndStore` 是否命中，只要收到帧就调用 `store.UpdateDcdwLabel(identity::FormatDcdwLabel(msg->sysid))`——`sysid` 在每条合法帧头里都有，不依赖识别出具体消息类型。
- `LogExtension` 加 5 个 OPEN_DRONE_ID_* 消息 id 的打印分支，外加 vendor_id/dcdw_label 单独打印一行方便真机验证。

## 6. 测试范围

- `test_extension_decoder.cpp` 加：
  - 5 个 OPEN_DRONE_ID_* 消息的解码测试（构造样例帧 → 解码 → 断言 struct 字段原样存入）。
  - `vendor_id` 提取测试：正常场景（uas_id 中间有 null）+ 边界场景（20 字节全部写满、无 null 终止符）。
- 新增 `test_identity.cpp`：
  - `FormatDcdwLabel`：sysid = 0 / 1 / 255 三个边界（0 位补零、正常值、最大值）。
  - `ReadRpiSerial`：用临时 fixture 文件测正常解析出 Serial 行，以及"文件里没有 Serial 行"两种情况——这个测试连同它依赖的 `path` 参数，属于第 4 节标注的"测试专用"范围，真机验证通过后要重新评估是否保留。

## 7. 全局约束（供实施计划引用）

- 官方 MAVLink struct 一律原样存储，不做单位换算（沿用 M3a/M3b 原则）。
- 涉及"字节数组转字符串"的地方（`uas_id` → `vendor_id`）必须用 `strnlen`，不能假设有 null 终止符。
- 不对 STM32 上报的任何身份数据做格式/取值校验——RPi 是使用方，不是校验方。
- 生产代码里任何"仅供测试注入"的参数/函数必须在声明处显式注明【测试专用】，并说明真实调用点从不覆盖默认值；对应的实施任务要记下"真机验证后重新评估去留"。
