# RTK 基站架设实验 Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** 在 CNS 箱内 Pi 的 F407 主流程增加 3DR/PX4 与 9Ps 两条链路，沿唯一箱子身份完成基站架设与状态上云；有合法缓存箱子绑定、无冲突时允许 F407 未连接的 RTK 发布。

**Architecture:** 同一 `main` 保留 F407 DeviceSession、绑定、MQTT、G2 和维护流程，按配置启用 RTK 附加模块。`serial` 属于 F407，可自动发现并排除 RTK 设备；`rtk_base.px4_device/px4_baud` 与 `device/baud` 分别独占 PX4、9Ps 设备。PX4 身份仅作实验元数据，外层/Topic/target 均使用 F407 箱子 `device_id`。遥测增加 `rtk={base,px4,box_identity_verified}`；缓存身份帧不携带旧 F407 普通传感数据。

**Tech Stack:** C++23、POSIX termios、官方 MAVLink common、u-blox F9P USB UBX/RTCM3、nlohmann/json、libmosquitto、CMake/CTest、Python 伪串口集成验证。

**Spec:** `docs/superpowers/specs/2026-11-06-rtk-base-station-design.md`

## Global Constraints

- F407/G2 与配置/控制流程保持；不改服务器或 F407。
- 只创建箱子 `cns_box` MQTT 会话。无持久化箱子绑定时仅本地观测；身份冲突不以缓存授权上报。
- 缓存箱子身份允许 RTK 帧，runtime 如实 cached/offline/unknown，不冒充 F407 实时在线；用 `box_identity_verified` 区分当前核验。
- 缓存 RTK 的 schema 3 快照保留精确空 `"drone_id":{"basic_id":{}}`，兼容现有服务器 `ParseTelemetry` 的对象合同；不填 `id_type`、`ua_type` 或 F407 实时观测值，schema 1 实时帧仍无 `drone_id`。不改服务器代码。
- 不修改生成头文件，不引入通用 RTK 解算或遥测补传框架。
- 入口一次校验；内部使用类型化数据。代码保持教学可读。
- 配置只写接收机 RAM，明确操作后执行；接收 ACK 与读回后才认配置完成。
- 室内、模拟、真实设备、云端、室外验收分别报告。

## Review Focus

- UBX/RTCM 混合、半帧、CRC 错误不产生有效状态。
- 端口打开不等于接收机响应；收到 RTCM 不等于 Survey-in 完成。
- 其他系统/冲突身份/心跳超时不能延用 PX4 实时身份，也不能改变箱子 MQTT 身份。
- 过期/错误目标/重复动作不能重复重启基站观测。
- 默认关闭和生产服务隔离避免占用实际串口或改变现有实验。

## 场景纠正后的任务

1. [x] 文档统一到单一 CNS 箱身份、三条独立串口及缓存箱子身份 RTK 发布；保留旧证据并标明范围。
2. [x] 将 RTK 接入现有主循环，移除独立 PX4 注册入口；保留 F407/G2/配置/控制与运行三态。
3. [x] 接入三端口配置与发现排除，两个 RTK 端口显式配置，F407 保持 `auto` 兼容。
4. [x] 统一箱子 Topic/target、遥测 `rtk` 对象和共享 ACK 队列；取消/到期处理不受 ACK 入队容量阻塞。
5. [x] RED/GREEN 验证 F407 在线、缓存身份、无绑定、冲突身份、PX4 掉线及旧 F407 数据不混入缓存帧。
6. [x] 全量构建/CTest、Pi 隔离构建与实际箱子身份 MQTT/HTTP 持久化；补最终结果和待验项，不提交或推送。

任务 1–6 已完成至缓存箱子身份的真实 Pi 与 MQTT/HTTP 验证，详见 [最新验证记录](../../validation/2026-11-06-rtk-box-cached.md)。WSL 全量 68/68 CTest（19.82 秒），Pi ARM64 相关测试 8/8，增量构建 0 条警告；隔离新版 SHA-256 为 `f66e617a7bbff24f54ec072a70c1746bb5bdc58c45f3854357c3455cfafe69c4`。下列历史完成项属于已被纠正的早期两串口实现，不能作为新拓扑完成标记。

专项修复：合法下限 `stale_after_ms=1000` 与固定 1Hz 的 MON-VER 查询竞争，在缓存箱子身份、F407 未连接且主循环约每 `100ms` 运行一次时可导致配置失败。查询间隔改为 `min(1000ms, stale_after_ms / 2)`，默认 `3000ms` 仍为 1Hz，下限 `1000ms` 为 2Hz。真实主程序 PTY 测试先 RED 再 GREEN，驱动 24 个用例/482 项断言通过，三条 PTY、`1000ms` 门限的真实主程序四阶段流程通过；未据此宣称实体硬件、Pi 原生、云端或室外定位验收。

后续 HTTP 核对发现，MQTT 遥测与 ACK 成功仍不能证明快照落库：缓存快照缺少 `drone_id.basic_id` 对象时被现有服务器解析器拒绝。服务器源码 probe 已验证补精确空对象后 accepted 且 payload unchanged；三 PTY 新增结构断言用旧二进制先 RED，修复后全量与 Pi 测试通过。最终真实 Pi 在 F407 未接时以合法缓存箱子 `DCDWCNS1GHC0G6LF8MY6` 唯一注册 `cns_box`，37 帧 RTK 遥测/44 条消息、两项动作及后续 8 条 1Hz 状态正常；正式箱子 HTTP 快照时间推进到 `2026-11-06T06:26:39.921Z`，包含 RTK 配置 `verified` 和 PX4 子 ID，证实空容器兼容有效。

上述隔离验证未替换正式程序，退出时原服务恢复 active，正式程序、配置、绑定前后摘要一致。随后常驻交付见下一节；F407 接回后的实体三链路/G2 同时运行、室外 Survey-in 成功、移动站 Fixed、网页实验页面仍未验或未做，任务完成不表示整体验收通过。

## 第一实验树莓派常驻交付

用户要求逐项完成，本轮先交付树莓派端常驻服务、MQTT 操作及现有 HTTP 状态记录；网页实验操作、实体 F407 接回验收和室外测量分别后续完成。沿用缓存箱子身份，不进入移动站固定解或 RTCM 转发。

- [x] 确认 canonical `/home/dcdw/cns_rpi` 无用户源码改动，正式配置和绑定可读；核对两个 USB 稳定路径。
- [x] 用已验证的新程序预检仅增加 `rtk_base` 的配置，保留其余现场配置。
- [x] 备份 canonical 源码、正式程序、配置及服务文件；同步当前源码与当前部署/迁移脚本。
- [x] 由 `dcdw` 在 canonical 路径执行唯一支持的 `scripts/deploy.sh`，启用 RTK 并保留运行。
- [x] 通过正式服务核验进程、箱子身份、基站识别、MQTT 查询/配置回执、HTTP 新快照及一次服务重启恢复。
- [x] 保存当前原生测试和部署记录，明确室内测量未完成的真实结果及后续待验项。

常驻结果见 [正式服务交付记录](../../validation/2026-11-06-rtk-base-service.md)。原生相关测试 8/8；正式服务 43 帧 RTK 遥测/50 条消息、两项动作和 HTTP 新快照通过；显式重启后查询再次完成，30 条 1Hz 状态正常，`NRestarts=0`。新程序保持 active/enabled，原绑定和非 RTK 配置不变。未提交或推送；室内测量仍无有效坐标，尚不能宣布整个实验验收通过。

## 历史组件阶段（独立 PX4 注册方案）

1. [x] 获取最新 main，创建分支，运行原有构建与 CTest 基线。
2. [x] 先写并执行基站解析/配置失败测试，再实现实际 F9P USB 适配；验证错误 CRC、分段输入、ACK/读回和 stale。
3. [x] 先写并执行 PX4 会话失败测试，再实现端点/真实身份/遥测来源/超时，保持 F407 注册不变。
4. [x] 先写并执行配置与动作入口失败测试，再实现默认禁用配置及类型化请求。
5. [x] 组合运行循环、现有格式 MQTT 与本地自检，编写使用真实可执行程序的伪串口+Broker 集成测试。
6. [x] 全量构建和 CTest；独立复审并修复实际发现问题。
7. [x] 识别实体 Pi 端口，独立 ARM64 构建与限时室内基站联调，记录真实证据及未验证项。无人机尚未上电，用户决定先收尾基站，PX4/真实 MQTT 实测延后。
8. [x] 更新设计、接口、变更记录，核对 Git diff，不自动合并 main 或发布版本。

历史结果：当时全量 67/67 CTest，Pi 原生 6/6；真实基站 RAM 配置读回 verified、NAV-SVIN active、RTCM 1230 三帧。验收范围及后续条件见 [室内验证记录](../../validation/2026-11-06-rtk-base-indoor.md)。这些数量对应旧源码快照，不能代表本轮三链路实现通过。

当日下午旧拓扑的追加验证：3DR 57600、实际 PX4 Basic ID、独立 PX4 注册的云端 MQTT 双向和正式 API 快照落库已验证；同时修复重启观测须等待新导航周期确认停止的问题。彼时修复后全量 67/67、Pi 相关 6/6、驱动 23 用例/468 断言通过，真机新一轮计时从 2 秒增长到 45 秒。详见[历史续验记录](../../validation/2026-11-06-rtk-cloud.md)。原始 JSON/JSONL 保留，不能用于宣称箱子身份、F407/G2 共存或缓存身份发布已通过。
