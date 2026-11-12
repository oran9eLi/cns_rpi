# Topic Report Rates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 服务器经已有配置指令热调整各数据 Topic 的上报周期，实验进行时可用并持久保存。

**Architecture:** 扩展配置入口的 Topic→毫秒补丁；纯调频复用持久化但不触发退出。已有遥测、状态、IMU 和 GNSS 发布点消费热更新的周期，保留各类消息语义。

**Tech Stack:** C++23、nlohmann/json、Mosquitto、doctest、Python 进程集成、CMake/CTest。

**Spec:** `docs/superpowers/specs/2026-11-12-topic-report-rates-design.md`

## Global Constraints

- 周期为 100～60000 整数毫秒，IMU 额外允许 0 恢复随新样本发送。
- 不修改 F407、服务器、生成文件，不部署，不推送。
- 不改变数据格式、身份门禁、QoS、retain、GNSS 完整性和 ACK 立即发送语义。
- 仅外部入口验证；内部使用已确定类型的周期。

## Review Focus

- 自定义快照/实时 Topic 后缀按实际名称寻址。
- 混合补丁和任意未知/命令 Topic 拒绝，整条补丁不部分应用。
- 写盘失败保持原频率；重复旧命令不覆盖后来设置。
- 实验进行中调频不重启、不取消采集，GNSS 满批/结束照常提前发送。
- IMU 只抑制多余新采样发布，不用旧样本填足目标频率，不改变有效期。

---

### Task 1: 配置入口、持久化与热生效结果

**Files:** `src/config_command/{command_parser,config_updater,command_processor,config_request}.{hpp,cpp}`；`src/config/app_config.{hpp,cpp}`；对应 test_config_*、test_command_processor 和 test_app_config。

**Interfaces:** ConfigParameterPatch.topic_publish_intervals_ms；可传快照/实时后缀的 ParseConfigCommand；接收已解析 ConfigCommand 的 ProcessConfigCommand；ReportingConfig 的三个毫秒周期。

- [x] 先在现有处理器测试加入成功调频不退出、保留无关字段、重复不重写的用例，运行并确认行为失败。
- [x] 实现参数校验、整条补丁持久化、热生效应答；覆盖自定义后缀、错误值/Topic/混合补丁、配置读取和写失败。
- [x] 扩展启动配置与设备来源请求构造，原配置缺省保持旧行为；运行相关测试。

### Task 2: 调度接入与实验不中断

**Files:** `src/telemetry/publisher.{hpp,cpp}`、`src/device_status/device_status.{hpp,cpp}`、`src/experiment/gnss_experiment.{hpp,cpp}`、`src/main.cpp`；相关单元和 gateway 测试。

**Interfaces:** Publisher.SetInterval(topic, interval)；PublicationState.SetInterval(interval)；GnssExperiment.SetBatchInterval(interval)；主循环只在已持久化 applied 时更新运行参数。

- [x] 添加真实发布调度、状态变化立即上报、GNSS 热改组批周期的失败测试。
- [x] 实现各调度器的热更新及 IMU 新样本最小发送间隔，保持默认每完整采样对发送。
- [x] 在 config/set 分支允许纯调频越过实验 busy 限制，使用已解析对象，不重新解析外部载荷。
- [x] 实际进程/PTY/broker 集成覆盖周期改变、PID 不变、持久化重启、GNSS 采集继续与原文完整。

### Task 3: 文档与整体验证

**Files:** `config/config.example.json`、`docs/Topic上报周期控制接口.md`、五份实验接口源文档，以及桌面对外文档。

- [x] 增加统一对接说明和各实验对周期控制的说明，只含接口、数据格式、流程。
- [x] 运行完整 Release 构建、CTest 和拨号测试；最终退出码 0，CTest 80/80、拨号测试 16/16，通过独立审阅修正后的完整回归。
- [x] 按 executing-plans 要求做一次最终独立代码审阅，实际问题的失败回归及针对性验证已完成，完整回归以最后运行结果为准。
- [x] 保留本地改动供用户后续提交/推送；本次不创建发布或现场部署。
