# 5G 链路自恢复与质量上报设计

## 1. 背景与目标

树莓派通过移远 RM500U-CNV 模块接入 5G 网络。现有
`cellular-dialup.service` 只在开机时执行一次拨号：拨号成功后脚本退出，运行期
无法发现或恢复断链；`cns_rpi` 只能根据 `usb0` 的载波、地址和路由判断 5G
状态，也没有向服务器上报无线信号质量。

本设计完成以下目标：

1. 由一个常驻服务统一负责首次拨号、运行期监测、断链恢复和质量采集。
2. 强制通过 `usb0` 验证 5G 公网连通性，避免 Wi-Fi 造成误判。
3. 将 5G 状态和质量加入既有 MQTT 遥测 JSON，不改变服务器路由协议。
4. STM32 继续只接收既有 `RPICELL` 在线位，不需要修改固件。
5. 保留现有 systemd 单元名称和部署入口，降低升级成本。

本轮不增加服务端数据库字段、质量历史、告警规则或前端页面，也不让守护服务
自行发布 MQTT。

## 2. 已验证的硬件事实

2026-07-22 在目标树莓派和正式物联网卡上完成只读实测：

- 模块型号：`Quectel RM500U-CNV`。
- 固件版本：`RM500UCNVAAR03A14M2G`。
- `AT+QCSQ` 返回失败，不作为采集接口。
- `AT+QENG="servingcell"` 可用，当前返回 `NR5G-SA` 及 RSRP、RSRQ、
  SINR。
- `AT+CSQ` 可用，用于补充 RSSI。
- `AT+COPS?` 可查询运营商，`AT+CGPADDR=<cid>` 可查询 PDP 地址。
- 物联网卡存在 IP 白名单约束。强制通过 `usb0` 实测：
  `112.124.52.232` 和 `119.29.29.29` 均连续 3 次可达；
  `112.124.52.232:1883` 可建立 TCP 连接。
- `223.5.5.5` 虽然可达，但实测出现 33% 丢包，不作为主要探测目标。

`AT+QENG` 字段按移远《RGx00U&RM500U Series AT Commands Manual》定义解析，
同时以本机真实固件响应作为兼容基线。

## 3. 总体架构

### 3.1 单一 AT 口所有者

现有 `scripts/cellular_dialup.py` 演进为常驻 5G 链路守护程序，继续由
`cellular-dialup.service` 启动。它是 RM500U AT 口的唯一所有者，串行执行：

- 动态发现 AT 口；
- 首次拨号；
- 网络和公网连通性检测；
- 信号质量查询；
- 断链重拨和模块软重启；
- 状态快照写入。

不再另建第二个 watchdog 进程，避免两个进程抢占串口、AT 响应串线以及状态源
不一致。

### 3.2 状态快照

守护服务将当前状态原子写入：

```text
/run/cns-rpi/cellular_status.json
```

写入时先在同目录生成临时文件，再使用原子重命名替换正式文件，保证
`cns_rpi` 不会读到半截 JSON。`/run` 随系统启动清空，适合存放运行期状态。

`cns_rpi` 不访问 AT 口，只读取快照并用于：

1. 生成 MQTT 遥测中的 `telemetry.cellular_5g`；
2. 生成发给 STM32 的既有 `RPICELL` 在线位。

这样服务器遥测与 STM32 屏幕使用同一个状态源。

## 4. 链路状态机

### 4.1 状态定义

| 状态 | 定义 |
| --- | --- |
| `UNKNOWN` | 服务刚启动、快照过期或尚无足够检测结果 |
| `ONLINE` | `usb0` 基础网络条件成立，两个公网目标均可达 |
| `DEGRADED` | 基础网络存在，但只有一个目标可达，或失败尚未达到离线阈值 |
| `OFFLINE` | 基础网络条件缺失，或两个目标连续 3 轮均不可达 |
| `RECOVERING` | 正在执行重新拨号、等待重新联网或软重启模块 |

基础网络条件包括：接口存在、载波正常、具有 IP 地址和经 `usb0` 的默认路由。
公网探测必须绑定 `usb0`，不得退回 Wi-Fi。

### 4.2 迟滞规则

- 默认每 10 秒检测一次。
- 两个目标连续 3 轮均失败，才从可用状态进入 `OFFLINE`。
- 恢复动作完成后，两个目标连续 2 轮均成功，才进入 `ONLINE`。
- 只有一个目标可达时进入 `DEGRADED`，不触发恢复。
- 第一版不根据 RSRP、RSRQ、SINR 或 RSSI 阈值主动重拨。无线指标只用于
  观测，避免弱信号现场反复重置模块。

### 4.3 探测目标

默认探测目标为：

1. `112.124.52.232`：CNS 服务器弹性公网 IP；
2. `119.29.29.29`：已通过正式物联网卡实测可达的独立公网地址。

只有两个目标在同一检测轮次中均失败，才累计一次双目标失败。MQTT 1883 端口
可作为诊断信息，但不单独触发模块恢复，避免服务端应用故障被误判为 5G 故障。

## 5. 恢复策略

进入 `OFFLINE` 后按以下阶梯恢复：

1. 进入 `RECOVERING`，重新执行 PDP 激活和 NCM 绑定。
2. 等待 `usb0` 恢复载波、地址和路由，再执行双目标探测。
3. 连续 3 次重新拨号仍未恢复时，执行一次 `AT+CFUN=0,1` 软重启 RM500U。
4. 软重启会使 USB 设备重新枚举；守护服务重新发现 AT 口，再执行首次拨号流程。
5. 仍失败时，按 15、30、60、120、240、最大 300 秒退避后继续恢复。
6. 双目标连续 2 轮成功后清零连续失败次数和退避级别。

恢复只作用于 RM500U 和其数据链路，不重启树莓派，不终止 `cns_rpi` 主程序。

模块拔出时立即记录 `present=false`，状态进入 `OFFLINE`，按检测周期重新发现；
模块插回后自动进入首次拨号流程。AT 命令失败只影响对应采集项或本次恢复，不能
导致守护进程因未捕获异常退出。

## 6. 质量采集与快照格式

快照字段如下：

```json
{
  "present": true,
  "link_state": "ONLINE",
  "operator": "China Mobile",
  "access_technology": "NR5G-SA",
  "ip_address": "100.77.73.48",
  "rsrp_dbm": -87,
  "rsrq_db": -10,
  "sinr_db": -3,
  "rssi_dbm": -75,
  "tx_bytes": 123456,
  "rx_bytes": 654321,
  "recover_count": 1,
  "last_recover_at": "2026-07-22T15:30:00.000+08:00",
  "last_error": null
}
```

字段来源和语义：

| 字段 | 来源与规则 |
| --- | --- |
| `present` | 能否发现并打开 RM500U AT 口 |
| `link_state` | 第 4 节定义的状态机结果 |
| `operator` | `AT+COPS?`；未知时为 `null` |
| `access_technology` | `AT+QENG="servingcell"`；未知时为 `null` |
| `ip_address` | `usb0` 当前 IP；没有地址时为 `null` |
| `rsrp_dbm` | `QENG` 的 RSRP；不可用时为 `null` |
| `rsrq_db` | `QENG` 的 RSRQ；不可用时为 `null` |
| `sinr_db` | `QENG` 的 SINR；不可用时为 `null` |
| `rssi_dbm` | `AT+CSQ` 按 `-113 + 2 × rssi` 换算；`99` 时为 `null` |
| `tx_bytes` | `/sys/class/net/usb0/statistics/tx_bytes` |
| `rx_bytes` | `/sys/class/net/usb0/statistics/rx_bytes` |
| `recover_count` | 本次系统启动期间累计发起的恢复动作数；服务重启从 `/run` 快照接续 |
| `last_recover_at` | 最近一次恢复动作时间；系统时间早于 2025-01-01 时为 `null` |
| `last_error` | 最近错误摘要；当前无错误时为 `null` |

信号质量默认每 30 秒采集一次。链路检测和快照刷新仍按 10 秒周期执行；不需要
每轮都发送多条 AT 查询。任一质量查询失败时，其他字段继续更新，失败字段为
`null`，同时更新 `last_error`。

## 7. MQTT 遥测兼容

`cns_rpi` 在既有 JSON 的以下位置加入快照对象：

```text
payload.telemetry.cellular_5g
```

示例：

```json
{
  "identity": {},
  "telemetry": {
    "cellular_5g": {
      "present": true,
      "link_state": "ONLINE",
      "operator": "China Mobile",
      "access_technology": "NR5G-SA",
      "ip_address": "100.77.73.48",
      "rsrp_dbm": -87,
      "rsrq_db": -10,
      "sinr_db": -3,
      "rssi_dbm": -75,
      "tx_bytes": 123456,
      "rx_bytes": 654321,
      "recover_count": 1,
      "last_recover_at": null,
      "last_error": null
    }
  }
}
```

服务端现有路由和数据库逻辑按完整 JSONB 保存最新遥测，因此无需认识这些字段
即可兼容。服务器和前端后续直接读取 `latest_telemetry.telemetry.cellular_5g`。

快照超过 30 秒未更新时，`cns_rpi` 输出 `link_state="UNKNOWN"`，保留最后一次
观测值，并用 `last_error` 明确标记快照过期。快照不存在或无法解析时也必须继续
发布其他遥测，不能因 5G 状态缺失丢弃整帧。

## 8. STM32 兼容

固件侧接口保持不变：

- 继续使用 MAVLink `NAMED_VALUE_INT`，名称为 `RPICELL`；
- 位图定义、发送周期和动态 sysid/compid 学习逻辑不变；
- 不向 STM32 发送运营商、接入制式或无线质量等详细字段；
- `ONLINE` 和 `DEGRADED` 均将既有 `RPICELL.ONLINE` 位置 1；
- `OFFLINE`、`RECOVERING` 和 `UNKNOWN` 将该位置 0。

因此本轮升级对 STM32 固件无感知，不要求固件侧重新开发。

## 9. 配置

守护服务与 `cns_rpi` 统一读取持久化配置：

```text
/var/lib/cns-rpi/config.json
```

在现有 `cellular` 节增加：

```json
{
  "probe_targets": ["112.124.52.232", "119.29.29.29"],
  "probe_interval_seconds": 10,
  "offline_failure_threshold": 3,
  "online_success_threshold": 2,
  "signal_sample_interval_seconds": 30,
  "redial_attempts_before_reset": 3,
  "recovery_delay_seconds": 15,
  "recovery_delay_max_seconds": 300
}
```

现有 APN、CID、USB 接口号和 AT 口等待时间字段继续使用。为兼容已经部署的持久
化配置，以上新增字段缺失时使用示例中的默认值；字段存在但类型错误、探测目标
为空、周期非正数或最大退避小于初始退避时，服务记录明确错误并非零退出，由
systemd 按服务失败处理，不能带着错误配置反复操作模块。

## 10. systemd 与日志

保留 `cellular-dialup.service` 名称，但将其从执行一次后保留状态的 oneshot 改为
常驻服务：

- 不再使用 `RemainAfterExit=yes`；
- 守护程序异常退出时由 systemd 重启；
- 正常的断链和恢复由进程内部状态机处理，不依赖 systemd 重启进程；
- 部署脚本和常用运维命令无需改名。

日志继续进入 journald，只记录服务启动、状态变化、恢复动作、退避变化和异常。
正常的 10 秒检测不逐轮打印，避免长期运行刷满日志。

## 11. 测试与验收

### 11.1 单元测试

- 配置默认值、合法性和错误提示；
- `QENG` 的 NR5G-SA、NR5G-NSA、LTE、未知及异常响应解析；
- `CSQ` 换算和未知值处理；
- 双目标探测结果到状态的映射；
- 离线 3 轮、上线 2 轮的迟滞；
- 三次重拨后软重启及指数退避上限；
- 快照序列化、原子替换、过期和损坏文件处理；
- `RPICELL` 对五种状态的映射；
- 遥测增加 `cellular_5g` 后不破坏既有字段。

### 11.2 无硬件集成测试

通过替身隔离 AT 串口、网卡、时钟和公网探测，覆盖首次拨号、瞬时丢包、持续
断链、软重启重新枚举、模块拔出与插回以及持续失败退避。

### 11.3 树莓派实机验收

1. 启动服务后自动完成拨号并保持常驻。
2. 快照每 10 秒刷新，质量字段每 30 秒更新。
3. 强制经 `usb0` 访问两个探测目标，Wi-Fi 不影响判断。
4. 模块拔出后进入离线，插回后自动发现并恢复拨号。
5. 模拟公网不可达，确认三轮失败后重拨，三次重拨失败后执行软重启。
6. 恢复后 MQTT 遥测包含完整 `telemetry.cellular_5g`。
7. STM32 继续按原协议显示 5G 在线状态，无需修改固件。

## 12. 明确不做

- 不新增服务端关系表、历史表或数据库列。
- 不把 5G 质量状态并入设备在线判定。
- 不实现服务端告警或前端图表。
- 不让 5G 守护服务直接连接 MQTT。
- 不因信号质量阈值主动重拨。
- 不重启树莓派或通过终止 `cns_rpi` 实现恢复。
