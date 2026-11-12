# BMI088原始与解析数据 树莓派MAVLink接口

版本v1，2026-11-10。STM32通过现有UART6同时上报BMI088原始寄存器采样、同次采样的解析六轴和设备状态。树莓派在现有MAVLink接收器中按下表分流；不要启动两个进程争读同一个串口。

## 1 连接与帧种类

UART6：PC6=STM32 TX、PC7=RX，115200、8N1、3.3V TTL，共地；沿用现有树莓派链路。外层为标准MAVLink2 `TUNNEL`，**msgid=385、CRC-extra=147**，源sysid为设备Node ID（1～250），compid=193，target_system/component=0/0。

| TUNNEL.payload_type | 十进制 | 内容 | 内层长度 | 默认节奏 |
|---|---:|---|---:|---|
| `0x8030` | 32816 | RAW 原始寄存器采样 | 64B | 与解析成对，最多5Hz |
| `0x8031` | 32817 | PARSED 解析六轴物理量 | 64B | 与RAW同源、同序号 |
| `0x8032` | 32818 | STATUS 设备与上报状态 | 64B | 约1Hz，无测量时仍报告 |

这些`payload_type`是本项目本地类型，**不是MAVLink消息号**。既有GNSS `0x8011～0x8014/0x8020`及其他遥测继续使用同一串口；原有`ATTITUDE #30`仍表示姿态估计，不与本次六轴解析包混用，其原发送节奏不变。

BMI088采集仍为标称100Hz；上报每200ms最多选择一个最新完整样本，生成RAW＋PARSED，**不是100Hz全量无损转发**。两包发送分属不同Comm轮次，其他MAVLink帧可插在中间。旧配对超过200ms、源撤销/换代或失败耗尽会取消，转发不补齐历史积压。

TUNNEL外层字段的小端布局为：偏移0 `u16 payload_type`、2 `u8 target_system`、3 `u8 target_component`、4 `u8 payload_length=64`、5 `u8 payload[128]`。先验MAVLink CRC，再恢复MAVLink2裁掉的尾零到标准TUNNEL字段容量133B，按`payload_length`取前64B内层。未用尾部应为0；**不得对内层或原始块执行rstrip**。有签名帧还带13B签名，无密钥不能声称验签通过。

**下文偏移均从TUNNEL.payload[0]开始；整数全部小端，i8/i16/i32为有符号二补码。** 不按C结构体内存布局解析。

## 2 三类帧公共头

| 偏移 | 类型 | 字段与含义 |
|---:|---|---|
| 0 | u8 | version，固定1 |
| 1 | u8 | kind：0=RAW、1=PARSED、2=STATUS，须与payload_type一致 |
| 2 | u8 | flags，见下 |
| 3 | u8 | bus：1=I²C、2=SPI；STATUS未知时可为0 |
| 4 | u32 | sample_sequence，驱动完整样本序号，0仅用于尚无样本 |
| 8 | u32 | sample_time_ms，采样服务入口的MCU开机毫秒，0合法 |
| 12 | u32 | encode_time_ms，本包实际编码时刻的MCU开机毫秒，重试会更新 |
| 16 | u32 | reinit_count，RAW/PARSED为该样本所属初始化次数；STATUS有诊断时为当前设备初始化次数 |

flags：bit0 `present`曾有完整样本；bit1 `usable`本包样本可用；bit2 `stale`样本过期或年龄未知；bit3 `invalidated`源被撤销/生命周期已变化；bit4～7保留0。`usable=1`必须同时满足present、年龄≤200ms且非stale/invalidated。没有授权的业务值用`null`，不能把wire中的零占位变成有效零值。有效的零角速度、零时刻及零加速度分量均合法。

编码年龄为`(encode_time_ms-sample_time_ms) mod 2³²`，只在差值≤`0x7FFFFFFF`时视为已知。RAW/PARSED实际只发送完整且新鲜的源；没有完整样本时不伪造零RAW，仅发送STATUS。STATUS中的4/8仍指最近源，16可已是新初始化次数，因此**STATUS不参与RAW/PARSED配对**。

## 3 0x8030 RAW64

| 偏移 | 类型 | 字段与含义 |
|---:|---|---|
| 20 / 21 | u8各1 | accel_addr_7bit / gyro_addr_7bit；I²C为0x18/0x19与0x68/0x69，SPI为0 |
| 22 / 23 | u8各1 | accel_chip_id=0x1E / gyro_chip_id=0x0F |
| 24 | u8 | 本轮ACC_STATUS（0x03）原值；bit7为新加速度就绪 |
| 25 | u8 | 本轮GYRO_FIFO_STATUS（0x0E）原值，无溢出，本轮帧数1～4 |
| 26 | u8 | gyro_frame_count，本轮实际读出的FIFO帧数1～4 |
| 27 | u8 | profile=1：已验证配置为加速度±12g、陀螺±1000°/s、ODR100Hz |
| 28～36 | u8[9] | 实际加速度寄存器0x12～0x1A返回字节；前6B为XYZ小端i16，后3B为小端24bit sensortime |
| 37～38 | u8[2] | 实际温度寄存器0x22/0x23字节，MSB在前 |
| 39～62 | u8[24] | 实际0x3F FIFO突发读取，前`frame_count×6`字节有效；每帧XYZ小端i16，未用尾部填0 |
| 63 | u8 | 保留0 |

“原始”是本轮真实读取的寄存器数据，**不含I²C地址/命令、SPI地址及无效dummy，不是总线波形抓包**；不从float反算。仅完整六轴及有效温度的采样轮次发布，初始化读写、失败事务或半帧不作为RAW样本。原始轴为芯片轴；两MEMS同轮读取不代表硬件同步。

原始解读：加速度`i16×12000/32768`得到mg；陀螺`i16×1000000/32768`得到mdps。解析包取本轮FIFO的**最后一帧**，前面的FIFO帧也保留在RAW中。sensortime分辨率39.0625µs，24bit约655.36s回绕，不能当UTC或完整开机时钟。

温度11bit值为`(MSB<<3)|(LSB>>5)`，大于等于1024时减2048；温度℃=`值×0.125+23`。MSB=0x80为无效，不能换算成正常温度。LSB低5位不参与换算，但原始字节仍必须保真，不要求低5位为0。该温度来自BMI088加速度部分，约1.28s更新，100Hz样本序号不代表温度100Hz更新。

## 4 0x8031 PARSED64

| 偏移 | 类型 | 字段与单位 |
|---:|---|---|
| 20 / 24 / 28 | i32各1 | accel_mg[X/Y/Z]，机体系加速度mg，1000mg=1g |
| 32 / 36 / 40 | i32各1 | gyro_mdps[X/Y/Z]，机体系角速度mdps，1000mdps=1°/s |
| 44 | i16 | temperature_cdeg，0.01℃ |
| 46 | u16 | sample_period_us，本地传感器发布间隔，µs；不是5Hz上报间隔 |
| 48 | u16 | accel_fullscale_mg，固定12000 |
| 50 | u16 | gyro_fullscale_dps，固定1000 |
| 52 / 53 / 54 | i8各1 | axis_map[X/Y/Z]，有符号1/2/3表示从芯片X/Y/Z取值及正负号 |
| 55 | u8 | axis_frame=1，机体系 |
| 56 | u32 | 该样本的fifo_drop_count，FIFO积压/溢出导致整批丢弃事件次数 |
| 60 | u32 | 该样本的driver_error_count，驱动错误累计次数 |

例如axis_map为`[2,-1,3]`表示机体X=芯片Y、机体Y=−芯片X、机体Z=芯片Z。必须为保持右手性的有符号轴排列，不接受重复轴或反射。当前默认`[1,2,3]`，以实际报文为准。

解析与屏幕六轴出口使用同一换算和映射，**不扣BIAS在估计器内部使用的偏置，不应用ZERO的主屏显示偏置**。它不是Roll/Pitch/Yaw。首次sample_period_us采用名义10000；现适配器在相邻本地发布间隔超过65ms时也回退名义10000，不能用此字段推算通信间隔或精确补算丢失运动。比较相邻上报时标应使用sample_time_ms差，并考虑抽样。

## 5 0x8032 STATUS64

| 偏移 | 类型 | 字段与含义 |
|---:|---|---|
| 20 / 24 | u32各1 | 当前driver_error_count / fifo_drop_count；诊断不可用时可为最近源报告值，消费者按可用标志区分 |
| 28 | u32 | source_published_count，Sensor发布到本上报邮箱的累计完整样本数 |
| 32 | u32 | source_overwrites，邮箱旧值尚未被Comm冻结确认就被新值覆盖的次数，包含主动抽样效果 |
| 36 | u32 | completed_pairs，本机在有效生命周期内完成两包UART提交的配对数，不证明Pi已收到 |
| 40 | u32 | abandoned_pairs，过期/撤销/换代/重试耗尽而取消的冻结配对数 |
| 44 | u32 | tx_failures，非OK的UART提交尝试数，不等同对端或空口丢包数 |
| 48 / 49 / 50 | u8各1 | 最近driver_stage / driver_result / bsp_result |
| 51 | u8 | device_ready，双ID及配置验证完成为1 |
| 52 / 53 | u8各1 | 当前accel/gyro ID，未就绪时保留实际错误ID，不强制为正确ID |
| 54 / 55 | u8各1 | 当前accel/gyro 7bit地址；SPI为0 |
| 56 | u32 | source_age_ms，未知/无样本为FFFFFFFF；已知年龄按公共头时标计算 |
| 60 / 61 | u8各1 | diagnostics_available / source_available，0或1 |
| 62 | u8 | Framework IMU状态：0未初始化、1启动、2在线、3降级、4离线、5失败、6关闭、255未知 |
| 63 | u8 | 保留0 |

stage：0无/成功、1总线、2等待电源/复位、3探测加速度、4探测陀螺、5复位、6配置、7验证、8读取、9重试等待。driver_result：0OK、1NO_DATA、2BAD_ID、3IO_ERROR、4TIMEOUT、5INVALID_PARAM、6CONFIG_ERROR。BSP：0OK、1ERROR、2BUSY、3TIMEOUT。诊断不可用时相应设备事实为未知，不把wire零当作真实成功。

所有累计计数均u32自然回绕。Comm重新Init保留Producer邮箱及源发布/覆盖计数，重置completed/abandoned/tx_failures，故计数下降不一定是整机重启。源序号缺口也不能直接解释为物理总线丢帧。

配对结果处理时还检查年龄和源生命周期；若UART提交返回时已超过200ms或源已撤销，该组计入abandoned，即使最后一包字节已经提交。以上是本机发送状态机计数，不能直接当作线上收到的帧对数；Pi应按自己的CRC、配对和有效性结果统计。

## 6 配对与接收端有效性

1. 先通过MAVLink CRC，按payload_type分流；RAW/PARSED以`(sysid,compid,reinit_count,sample_sequence,sample_time_ms)`配对，**不用8bit MAVLink sequence配对**。两包允许交错/先后到达，重试相同内容不重复发布。
2. 原始和解析必须是同一总线、同一key。可用原始最后FIFO帧及axis_map独立复算，和解析mg/mdps/cdeg比较；参考解码器允许整数舍入带来的1个最小单位差异，超差拒绝授权。
3. 仅两包均usable且接收端已知年龄≤200ms时使用工程量。将每包编码年龄加上Pi从收到该包起的等待时间，断流后主动将value置null。没有同步时钟，无法由此扣除未知串口/网络单向延迟，不宣称端到端精密同步。
4. 缺一包在Pi本地1s后清理，不与下一样本拼接；保留原始字节可用于审计，不能给导航/姿态业务续期。STATUS的source_available=0会撤销本地最新业务值。
5. v1没有保证唯一的boot ID。Pi连接重建、已知设备重启或计数/时钟回绕时，清理该源未完成配对及旧缓存，服务端可增加自己的connection_epoch。参考解码器可识别明显uptime后退/计数重置，但不能保证识别全部重启和延迟旧包。

发送最多尝试3次，失败后至少等待50ms；成功只表示提交到现有UART发送流程，无Pi应用级ACK。无新完整样本时不重复发送旧测量对，STATUS继续报告设备状态。

## 7 树莓派接入代码与验证入口

参考代码：`Tools/bmi088_uart6_decoder.py`；机器字段表：`Tools/bmi088_uart6_v1.json`。离线解析不需要第三方库：

JSON中的`golden_vectors`为真实C编码器与MAVLink库产生的主机黄金帧，已由Python校验CRC并配对解码；它们不是硬件实测数据，可用于Pi接入自测。

```sh
python3 Tools/bmi088_uart6_decoder.py --input capture.bin --pairs-only
python3 Tests/Unit/test_bmi088_uart6_decoder.py
```

将参考解码器放入Pi程序可导入的目录。如现有Pi已用pymavlink，继续复用其唯一串口读取器，在已验CRC的TUNNEL对象处分流：

```python
import time
from bmi088_uart6_decoder import decode_tunnel, PairAssembler, ProtocolError

pairs = PairAssembler()
# msg来自已通过CRC校验的pymavlink接收器，不是自行构造的未验证对象。
now_ms = time.monotonic_ns() // 1_000_000
try:
    packet = decode_tunnel(msg, crc_verified=True)  # 非BMI类型返回None
    if packet is not None:
        for event in pairs.feed(packet, now_ms):
            if event["event"] == "bmi088_pair":
                print(event["identity"], event["value"])
except ProtocolError as exc:
    # 接入现有错误日志，丢弃该异常包，不终止整个串口读取循环。
    print("BMI088协议错误", str(exc))
# 即使UART没有新包，也要定期清理、按本机时间重新判断有效性。
pairs.expire(now_ms)
latest = pairs.current(msg.get_srcSystem(), msg.get_srcComponent(), now_ms)
# latest为None或latest["value"]为None时，上位机相应工程量应标记无效。
```

直接读串口的独立诊断模式可用`--port /dev/serial0 --baud 115200 --pairs-only`，仅该模式需要pyserial；不要与既有串口服务同时运行。MAVLink2签名仅识别帧长度，解码器始终明确`signature_verified=False`。工具不会发送命令、配置或心跳。
