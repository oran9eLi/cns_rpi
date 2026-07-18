# STM32 与树莓派链路周期通信约定

版本：V1.0  
日期：2026-07-18  
适用链路：STM32 USART6 ↔ 树莓派 UART（MAVLink v2）

## 1. 目的

本约定明确 STM32 与树莓派之间 HEARTBEAT、遥测数据和 5G 状态
`RPICELL` 的职责边界。

两端不建立额外的启动握手状态机，不因收到某一帧而停止或启动另一类
周期帧。HEARTBEAT、遥测数据和 `RPICELL` 相互独立，不互相代替。

## 2. 基本原则

1. STM32 上电后按固件现有调度持续发送 HEARTBEAT 和遥测数据。
2. STM32 不等待树莓派或 5G 网络就绪，也不因收到 `RPICELL` 而停止
   HEARTBEAT。
3. 树莓派从 STM32 首条合法 HEARTBEAT 学习动态 `sysid`。
4. 学习完成后，树莓派按配置周期持续发送自身 HEARTBEAT 和
   `RPICELL`。
5. 5G 未连接时树莓派仍发送 `RPICELL`，其 `ONLINE` 位为 0；不以“不发帧”
   表示 5G 离线。
6. 遥测数据帧可用于判断 UART 上仍有数据活动，但不代替 HEARTBEAT 的
   端点发现职责。

## 3. MAVLink 身份

### 3.1 STM32

- `sysid`：由 STM32 UID 派生，范围 `1..250`，不同设备可能不同。
- USART6 帧头 `compid`：`193`。
- HEARTBEAT payload：
  - `type=MAV_TYPE_ONBOARD_CONTROLLER(18)`；
  - `autopilot=MAV_AUTOPILOT_INVALID(8)`。

### 3.2 树莓派

- `sysid`：不写死，使用从 STM32 HEARTBEAT 学习到的同一动态 `sysid`。
- `compid`：`MAV_COMP_ID_ONBOARD_COMPUTER(191)`。
- 学习完成前，不向 STM32 发送树莓派 HEARTBEAT、`RPICELL` 或控制命令。
- 单次进程运行期内不切换已学习的 STM32 端点。更换 STM32 后重启
  树莓派程序重新学习。

STM32 和树莓派属于同一设备，共用 `sysid`，通过 `compid=193/191`
区分组件。

## 4. 正常通信流程

```text
STM32上电
  → STM32持续发送HEARTBEAT和遥测数据
  → 树莓派从HEARTBEAT学习动态sysid
  → 树莓派持续发送自身HEARTBEAT和RPICELL
  → STM32根据RPICELL.ONLINE更新5G显示状态
  → 两端继续各自的周期发送，不切换发送模式
```

树莓派是否已经连接 5G 不影响 STM32 遥测帧的发送。树莓派在 5G 恢复后
自然恢复 MQTT 上传，无需通知 STM32 切换数据模式。

## 5. `RPICELL` 状态处理

`RPICELL` 使用 MAVLink `NAMED_VALUE_INT`，详细位图见《5G 连接状态
`RPICELL` 固件对接说明》。

STM32 的显示逻辑：

| 条件 | 建议显示 |
|---|---|
| 从未收到 `RPICELL` | 5G 状态未知/未连接 |
| 收到 `RPICELL`，`ONLINE=0` | 5G 未连接 |
| 收到 `RPICELL`，`ONLINE=1` | 5G 已连接 |
| 超过 5 秒未收到 `RPICELL` | 5G 状态未知/未连接 |

`RPICELL` 的默认发送周期为 1000 ms，由树莓派
`cellular.heartbeat_interval_ms` 配置。固件侧建议使用 5000 ms 超时，
既能容忍短暂调度抖动，也能在树莓派退出、掉电或 UART 中断后及时
恢复离线显示。

## 6. 故障与恢复

### 6.1 5G 掉线

树莓派继续发送 `ONLINE=0` 的 `RPICELL`。STM32 更新显示，心跳和遥测
发送逻辑不变。

### 6.2 树莓派程序重启

STM32 仍持续发送 HEARTBEAT，新进程可重新学习 `sysid`。学习完成后恢复
树莓派 HEARTBEAT 和 `RPICELL`。

### 6.3 UART 中断或树莓派掉电

STM32 通过 `RPICELL` 超时将 5G 状态置为未知/未连接，但不停止
HEARTBEAT 和遥测数据。链路恢复后无需额外握手。

### 6.4 STM32 重启

实地部署不支持运行期热换 STM32。如 STM32 重启且树莓派程序仍在运行，
因同一块 STM32 的 UID 派生 `sysid` 不变，链路可继续工作。更换不同
STM32 时必须重启树莓派程序。

## 7. 不采用的行为

为避免两端状态机互相等待，明确不采用以下行为：

- STM32 收到 `RPICELL` 后停止 HEARTBEAT；
- 用遥测数据帧完全代替 HEARTBEAT；
- STM32 等待 5G 在线后才开始发送遥测；
- 树莓派仅在 5G 在线时发送 `RPICELL`；
- 通过不发 `RPICELL` 来表示 5G 离线。

## 8. 联调验收

1. STM32 上电后持续发送 `sysid=<UID派生值>`、`compid=193` 的
   HEARTBEAT。
2. 树莓派收到首条合法 HEARTBEAT 后使用同一 `sysid`、`compid=191`
   发送自身 HEARTBEAT 和 `RPICELL`。
3. 5G 未连接时 STM32 持续收到 `ONLINE=0`，显示为未连接。
4. 5G 连接后 STM32 在下一个 `RPICELL` 周期内收到 `ONLINE=1`
   并更新显示。
5. 5G 再次掉线后 STM32 收到 `ONLINE=0` 并更新显示。
6. 停止树莓派程序或断开 UART，STM32 在 5 秒内将 5G 状态置为未知/
   未连接，同时仍持续发送 HEARTBEAT 和遥测数据。
7. 恢复树莓派程序或 UART 后，两端无需额外握手即恢复正常通信。
