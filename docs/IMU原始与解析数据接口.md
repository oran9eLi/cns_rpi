# IMU 原始与解析数据接口

版本：v1，2026-11-12。状态：**Pi 侧已实现，本地软件验证；实机与服务器联调待验**。

本文确定 BMI088 数据从树莓派到服务器的 MQTT 合同。F407 线协议依据随附的 [BMI088 UART6 接口 v1](protocols/BMI088原始与解析数据_树莓派MAVLink接口.md)。所有 JSON 和十六进制样例均为人工构造的说明数据，不是黄金帧或实机采集证据。

## 1. 接法与范围

采用“解析数据复用普通遥测、完整采样对独立发布”的接法，由本次接口设计选定：

| 接法 | 取舍 |
|---|---|
| 全部放入现有遥测 | 接入集中，但实时周期会重复携带同一对原始字节。 |
| 解析值进入现有遥测，采样对走独立 Topic（采用） | 设备总览和曲线沿用现有链路，原始对照按新样本发布。 |
| 复用 GNSS 式开始／停止采集与本地归档 | 适合按轮次保存实验，但本次仅定义持续观测上行，无需增加采集动作和归档状态机。 |

Pi 复用现有 C++ 服务的唯一 F407 MAVLink 接收器，在已经核验帧 CRC、当前设备身份和来源端点后处理 `0x8030/0x8031/0x8032`。参考 Python 解码器和机器字段表是交叉校验资料，**不是运行依赖或实施前置条件**。既有 MAVLink 库负责外层解码和尾零恢复。

F407 标称 100Hz 采集，最多 5Hz 选择最新样本发送 RAW＋PARSED，STATUS 约 1Hz。Pi 的重复遥测发布不会生成新的传感器样本。六轴与现有 `telemetry.attitude` 分别保留，常规十一字段设备状态帧继续承担设备资料和健康概览。

本版按 F407 自然上报持续处理，采样对逐对发布；服务器按需订阅和保存。没有“服务器档案完整”或“100Hz 无损采集”的承诺。

## 2. Topic、频率和发布条件

沿用现有 MQTT 地址、认证和 `namespace`，默认 `namespace=cns_rpi`。`device_id` 是 F407 箱子 SN，不是 MAVLink Node ID。

| 用途 | Topic | 节奏 | QoS | retain |
|---|---|---|---:|---|
| 完整遥测与 IMU 诊断 | `{namespace}/{device_id}/telemetry/snapshot/v1` | 沿用快照周期，默认 1Hz | 0 | false |
| 实时解析六轴 | `{namespace}/{device_id}/telemetry/realtime/v1` | 沿用实时周期，默认 10Hz；新样本最多 5Hz | 0 | false |
| 原始／解析采样对 | `{namespace}/{device_id}/imu/sample/v1` | 每个完整采样对至多一条，来源最多 5Hz | 0 | false |

前两个 Topic 的外层 schema 保持快照 `3`、实时 `1`，新增 `telemetry.imu`。原字段、通道开关、周期和身份门禁保持原约定。快照包含 IMU STATUS；实时不携带 STATUS 诊断和原始字节。

新 `imu/sample/v1` 仅在当前 F407 身份已核验、MQTT 可用、完整配对且解码与原始复算一致时发送。不为缺包补造采样对；相同采样 key 的重试不重复发布。发布失败丢弃当前记录，重连后接收新的采样对，不积压或补发历史。它与遥测通道开关分别工作，本版没有额外采集开关或开始／停止命令。

普通遥测只保留最新 IMU 状态，不保存测量历史；F407 整链路失效时沿用既有遥测门禁，服务器通过第 5 节的有效期清除当前值。合法缓存 SN 不代表有当前 IMU 测量。

## 3. 公共 IMU 对象

三类上行均使用相同的来源与采样字段：

| 字段 | 类型 | 含义 |
|---|---|---|
| `sensor` | string | 固定 `BMI088`。 |
| `source_epoch` | string | Pi 生成的 UUID，表示本次串口来源会话；不是 F407 boot ID。 |
| `source.system_id` / `component_id` | integer | 当前已核验的 MAVLink 端点；组件为 193，系统号以实际 Node ID 为准。 |
| `sample` | object / null | 最近完成的采样对；尚未完成任何配对时为 null。 |
| `status` | object / null | 最近解码的 STATUS，首次尚未收到为 null；仅快照携带。 |

首次尚未收到任何 BMI088 帧时，普通遥测省略 `telemetry.imu`。收到 STATUS 但尚无完整采样对时，输出 `sample=null` 和真实 STATUS，不填全零六轴。

### 3.1 来源会话与采样 key

Pi 进程启动、串口重连、身份／来源端点切换、确认 F407 重启，以及检测到采样序号或 MCU 时钟回退／回绕时，更换 `source_epoch`，清除该来源的未完成配对和最新业务值。MQTT 重连本身不更换仍连续工作的串口来源会话。STATUS 的 Comm 计数下降不能单独当作整机重启。

配对使用线协议 key：

`(system_id, component_id, reinit_count, sample_sequence, sample_time_ms)`。

服务器去重或建立曲线样本再加上 `device_id` 和 `source_epoch`。MAVLink 8 位 sequence、MQTT 实时帧的 `sequence` 均不作为传感器采样序号。v1 不能识别所有未被观测到的 F407 重启；`source_epoch` 不提供固件唯一启动标识的保证。

### 3.2 sample 字段

| 字段 | 类型 | 含义 |
|---|---|---|
| `sample_sequence` | integer | F407 完整样本序号，不因 Pi 再次发布而改变。 |
| `sample_time_ms` | integer | MCU 开机采样时刻；0 合法，不是 UTC。 |
| `reinit_count` | integer | 该样本所属初始化次数。 |
| `raw_encode_time_ms` / `parsed_encode_time_ms` | integer | 两包各自实际编码的 MCU 开机时刻。 |
| `bus` | string | `i2c` 或 `spi`。 |
| `axis_map` | integer[3] | 有符号轴排列，例如 `[2,-1,3]`；按线协议校验右手性。 |
| `axis_frame` | string | 固定 `body`，机体系。 |
| `valid` | boolean | Pi 本次发布时该采样对是否仍获准作为当前工程量。 |
| `invalid_reason` | string / null | 有效为 null；失效见第 5 节。 |
| `age_ms` | integer / null | 本次发布时的已知采样年龄；未知为 null。 |
| `valid_for_ms` | integer | 本次发布时剩余可用时间；无效为 0。 |
| `value` | object / null | 有效时的工程量；无效必须为 null，元数据可保留用于说明上次来源。 |

`value` 的字段固定为：

| 字段 | 类型 | 单位／含义 |
|---|---|---|
| `accel_mg` | integer[3] | 机体系 XYZ 加速度，1000mg=1g。 |
| `gyro_mdps` | integer[3] | 机体系 XYZ 角速度，1000mdps=1°/s。 |
| `temperature_cdeg` | integer | 温度，100 个单位=1℃。 |
| `sample_period_us` | integer | 本地传感器发布间隔，不是 MQTT 间隔。 |
| `accel_fullscale_mg` | integer | v1 为 12000。 |
| `gyro_fullscale_dps` | integer | v1 为 1000。 |
| `fifo_drop_count` / `driver_error_count` | integer | 该样本携带的累计计数，来源为 PARSED，不表示 Pi 或网络丢包。 |

Pi 保留 F407 的整数单位；Web 可显示 g、°/s 和 ℃，相应除以 1000、1000 和 100。有效的零角速度和零分量仍为数值 0。这里没有姿态估计器 BIAS 或主屏 ZERO 显示偏置。

## 4. 每个 Topic 的 JSON 示例

三个示例描述同一人工构造样本：芯片 Z 原始计数 2731，换算约 1000.122mg，解析整数为 1000mg；陀螺为零，温度原字节为 `0000`，对应 23℃。

### 4.1 实时：telemetry/realtime/v1

以下为带 IMU 的实时帧示例，其他已经收到的快变遥测仍按既有合同同时输出：

```json
{
  "schema_version": 1,
  "device_id": "DCDWCNS1S2MEAG1VTA0C",
  "sequence": 200,
  "sent_at": "2026-11-12T02:00:00.020Z",
  "telemetry": {
    "imu": {
      "sensor": "BMI088",
      "source_epoch": "9f54bdc3-3a55-4f21-9ae8-63c4a0f4d624",
      "source": {"system_id": 218, "component_id": 193},
      "sample": {
        "sample_sequence": 42,
        "sample_time_ms": 10000,
        "reinit_count": 1,
        "raw_encode_time_ms": 10005,
        "parsed_encode_time_ms": 10008,
        "bus": "i2c",
        "axis_map": [1, 2, 3],
        "axis_frame": "body",
        "valid": true,
        "invalid_reason": null,
        "age_ms": 20,
        "valid_for_ms": 180,
        "value": {
          "accel_mg": [0, 0, 1000],
          "gyro_mdps": [0, 0, 0],
          "temperature_cdeg": 2300,
          "sample_period_us": 10000,
          "accel_fullscale_mg": 12000,
          "gyro_fullscale_dps": 1000,
          "fifo_drop_count": 0,
          "driver_error_count": 0
        }
      }
    }
  }
}
```

### 4.2 快照：telemetry/snapshot/v1

此例只展示 IMU 增量和快照必要信封。实际快照保留已经收到的其他遥测、身份和模块等字段。

```json
{
  "schema_version": 3,
  "device_id": "DCDWCNS1S2MEAG1VTA0C",
  "device_type": "cns_box",
  "school_name": "示例职业技术学院",
  "sent_at": "2026-11-12T02:00:00Z",
  "telemetry": {
    "imu": {
      "sensor": "BMI088",
      "source_epoch": "9f54bdc3-3a55-4f21-9ae8-63c4a0f4d624",
      "source": {"system_id": 218, "component_id": 193},
      "sample": {
        "sample_sequence": 42,
        "sample_time_ms": 10000,
        "reinit_count": 1,
        "raw_encode_time_ms": 10005,
        "parsed_encode_time_ms": 10008,
        "bus": "i2c",
        "axis_map": [1, 2, 3],
        "axis_frame": "body",
        "valid": true,
        "invalid_reason": null,
        "age_ms": 20,
        "valid_for_ms": 180,
        "value": {
          "accel_mg": [0, 0, 1000],
          "gyro_mdps": [0, 0, 0],
          "temperature_cdeg": 2300,
          "sample_period_us": 10000,
          "accel_fullscale_mg": 12000,
          "gyro_fullscale_dps": 1000,
          "fifo_drop_count": 0,
          "driver_error_count": 0
        }
      },
      "status": {
        "received_age_ms": 0,
        "stale": false,
        "flags": {"present": true, "usable": true, "stale": false, "invalidated": false},
        "bus": "i2c",
        "sample_sequence": 42,
        "sample_time_ms": 10000,
        "encode_time_ms": 10010,
        "reinit_count": 1,
        "source_age_ms": 10,
        "diagnostics_available": true,
        "source_available": true,
        "device_ready": true,
        "framework_state": 2,
        "accel_chip_id": 30,
        "gyro_chip_id": 15,
        "accel_addr_7bit": 24,
        "gyro_addr_7bit": 104,
        "driver_stage": 0,
        "driver_result": 0,
        "bsp_result": 0,
        "driver_error_count": 0,
        "fifo_drop_count": 0,
        "source_published_count": 42,
        "source_overwrites": 39,
        "completed_pairs": 3,
        "abandoned_pairs": 0,
        "tx_failures": 0
      }
    }
  },
  "drone_id": {"basic_id": {"id_type": 1, "ua_type": 0}}
}
```

STATUS 字段沿用 F407 定义，Pi 只做以下明确转换：

- `flags` 展开原 bit0～3；`diagnostics_available`、`source_available` 和已知的 `device_ready` 转为布尔值。
- `received_age_ms` 是 Pi 收到该 STATUS 后经过的单调时间；超过 3000ms 则外层 `status.stale=true`。`flags.stale` 则是 F407 对测量源的判断，两者含义不同。
- `bus=0`、`framework_state=255`、`source_age_ms=0xFFFFFFFF` 分别输出 null；其余 0～6 模块状态保留数值。
- `flags.present=false` 时，STATUS 的 `sample_sequence`、`sample_time_ms`、`source_age_ms` 输出 null。存在源时采样时刻 0 合法。
- `diagnostics_available=false` 时，当前设备事实 `device_ready`、`reinit_count`、芯片 ID、I²C 地址、`driver_stage/result`、`bsp_result`、当前驱动错误与 FIFO 丢弃计数均输出 null。后两项仍可在已配对 sample 中以该样本的原值出现。
- SPI 地址输出 null；I²C 的实际错误 ID 原样保留，不强行改为预期 ID。存在、就绪、诊断可用分别判断。
- `source_published_count`、`source_overwrites`、`completed_pairs`、`abandoned_pairs`、`tx_failures` 为收到的 Comm 计数，不依赖诊断是否可用。STATUS 过期时保留最后报告供排障，消费者必须连同 `status.stale` 展示，不能当作当前事实。
- `source_overwrites` 包含从 100Hz 采集中主动抽样为最多 5Hz 的覆盖，正常运行也会增长，不作为物理总线丢帧计数。
- `driver_stage`、`driver_result`、`bsp_result` 枚举和值域以 F407 文档为准；计数按 u32 自然回绕，不从序号缺口推断丢包率。

STATUS 不参与采样对配对，不把其当前初始化次数覆盖到旧 sample；其在线、错误计数和发送成功数也不能证明 Pi 已收到完整的新测量。

### 4.3 采样对：imu/sample/v1

每条记录包含一对完整的 64B 内层 payload 和同一对的解析结果，便于服务器直接展示换算、保存或教学对照，不需另写一个 F407 解码器才能画六轴曲线：

```json
{
  "schema_version": 1,
  "device_id": "DCDWCNS1S2MEAG1VTA0C",
  "sent_at": "2026-11-12T02:00:00.020Z",
  "imu": {
    "sensor": "BMI088",
    "source_epoch": "9f54bdc3-3a55-4f21-9ae8-63c4a0f4d624",
    "source": {"system_id": 218, "component_id": 193},
    "sample": {
      "sample_sequence": 42,
      "sample_time_ms": 10000,
      "reinit_count": 1,
      "raw_encode_time_ms": 10005,
      "parsed_encode_time_ms": 10008,
      "bus": "i2c",
      "axis_map": [1, 2, 3],
      "axis_frame": "body",
      "valid": true,
      "invalid_reason": null,
      "age_ms": 20,
      "valid_for_ms": 180,
      "value": {
        "accel_mg": [0, 0, 1000],
        "gyro_mdps": [0, 0, 0],
        "temperature_cdeg": 2300,
        "sample_period_us": 10000,
        "accel_fullscale_mg": 12000,
        "gyro_fullscale_dps": 1000,
        "fifo_drop_count": 0,
        "driver_error_count": 0
      }
    },
    "raw_payload_hex": "010003012a00000010270000152700000100000018681e0f8001010100000000ab0a000000000000000000000000000000000000000000000000000000000000",
    "parsed_payload_hex": "010103012a0000001027000018270000010000000000000000000000e8030000000000000000000000000000fc081027e02ee803010203010000000000000000"
  }
}
```

两个 hex 字符串均固定为 **128 个小写十六进制字符，分别代表完整 64B**；没有空格或 `0x` 前缀，保留尾零，包含对应公共头。它们不是完整 MAVLink wire 帧或总线波形：外层 sysid/compid 已在 `source` 中，寄存器块的精确偏移见 F407 接口。

这里的值在首次形成采样对时即已按第 5 节判断。若已不可用，仍可发布完整配对的两个 payload 作观测记录，但 `sample.valid=false`、`value=null`；不把过期包重新授权。格式不合法、配对冲突或原始复算不一致的包不进入该 Topic，记录在 Pi 解码日志中。

## 5. 有效性与服务器显示规则

1. Pi 入口一次完成版本、类型、长度、范围、轴映射和同源配对校验。RAW 与 PARSED 允许交错，最多保留 1 秒的未完成配对；只有一包不产生 sample。RAW 最后一帧 FIFO 经映射和换算后，与 PARSED 加速度、角速度和温度允许 1 个整数最小单位的舍入差异。
2. 每包编码年龄按 u32 模差计算；大于 `0x7FFFFFFF` 视为未知。`sample.age_ms` 为两包各自“编码年龄＋Pi 收到后等待时间”的较大值。Pi 使用单调时钟计算，普通遥测每次序列化前重新求值。
3. 只有两包均 `present/usable`、非 stale/invalidated，来源未撤销且已知年龄不超过 200ms，`sample.valid` 才为 true。`valid_for_ms=max(0,200-age_ms)`；无效时为 0，`value=null`。STATUS 或重复帧不会刷新原采样时刻。
4. 失效原因为 `age_unknown`、`sample_stale`、`source_unavailable`；当前源被撤销／标记 invalidated 或收到 STATUS `source_available=0`，立即撤销缓存业务值。之后完整的新采样对可以重新建立有效值，重复旧配对不能恢复授权。
5. MCU 开机毫秒、Pi UTC `sent_at`、Pi 单调时钟分别使用。上述年龄没有包含不可测的串口和网络单向延迟，**不是精确端到端年龄或同步采样保证**，也不凭这些流量重建 100Hz 无损运动。
6. 服务器／Web 每个采样 key 只建立一次当前值有效期：首次接收时最多保留 `valid_for_ms`。同 key 再次出现不能延长第一次建立的有效期；收到更短的剩余期限或显式失效时只可缩短／清除。有效期为 0 立即清除。快照和实时重复携带同一 key 时也遵循此规则。
7. 服务器转给 Web 的剩余有效期应扣除自己已知的本地等待时间；Web 同样用本地单调计时过期清空。端到端传输年龄仍受第 5 条限制。历史记录可以保留旧数值和原文，当前仪表与曲线不得把旧采样标为新测量。
8. `sample=null`、`value=null`、来源会话更换、MQTT／页面断开时清除当前有效显示。若 Pi 因既有门禁暂停遥测，服务器也会在有效期耗尽后清除，不依赖额外一条失效通知。

例如同一 sample 经过 230ms 后，元数据仍可保留，但以下是其更新后的有效性字段片段：

```json
{
  "valid": false,
  "invalid_reason": "sample_stale",
  "age_ms": 230,
  "valid_for_ms": 0,
  "value": null
}
```

STATUS 的 `device_ready=true`、`framework_state=2`、`source_available=true` 不会单独使 `sample.valid=true`。反过来，STATUS 暂未刷新也不撤销一个有独立新鲜证据的新采样对。

## 6. 服务器接入职责

- 普通遥测解析器接纳新增 `telemetry.imu`，快照存最新诊断与概览，实时用于当前设备的六轴显示。1Hz 快照不承担持续 200ms 有效窗口的实时仪表刷新。
- 需要原始对照或历史时，显式订阅 `{namespace}/{device_id}/imu/sample/v1`；需要后台归档全部设备时才使用 `{namespace}/+/imu/sample/v1`。broker 权限需包含新 Topic。
- 每个 MQTT 入口核对 Topic 设备段与 `device_id`、schema 和字段类型，内部传递已解析对象。普通遥测的外层合同保持原样，不用内层 sysid 代替稳定 SN。
- 六轴曲线按采样 key 去重，`sample_time_ms` 用于同一来源会话内的采样间隔；不按实时 MQTT 包数计算采样率。采样对 Topic 与普通遥测中的同 key 也不是两次采样。
- 原始归档由服务器负责。QoS 0 无投递或存档确认，允许网络丢失，不计算虚假的连续性；需要可靠按轮次采集时另行设计归档流程，不能将本实时接口描述为该功能已存在。
- 原有 14 模块表继续决定常规 `device_status/fault_modules`。本 IMU STATUS 作为诊断明细，不另写一套健康累计判定。

## 7. 实现与验证边界

Pi 已接入唯一 MAVLink 接收器，完成三种 wire 解码、原始复算、同源配对、有效期计算和三个 MQTT 输出。本次没有修改服务器、F407 或现场设备，没有新增 Python 运行依赖。

本地验证使用人工构造的 F407 数据、实际 `cns_rpi` 进程、PTY 串口和隔离 Mosquitto；文档三个 JSON 示例与固定合同逐项比对，实际发布覆盖正常值、过期、来源撤销、身份门禁、独立上传与 MQTT 重连。代码入口和验证命令见 [实施与软件验证记录](validation/2026-11-12-bmi088-uplink.md)。

上述本地验证不包含 ARM64 构建、树莓派安装、真实 F407/BMI088 采样或服务器/Web 联调。ARM64 安装包以本提交对应的 GitHub Actions CI 结果为准；真实互操作仍应使用 F407 编码器帧或单串口读者抓取的实物报文验证，人工构造样例或软件测试通过不替代此证据。
