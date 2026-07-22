#pragma once

/**
 * @file json_serializer.hpp
 * @brief 把 state::TelemetryState 转成人类可读、单位换算过的 JSON。
 *
 * @details
 * 只依赖 state::TelemetryState，不依赖 config::AppConfig 整个结构体（school_name
 * 作为独立参数传入），也不依赖 protocol/ 解码层——保持"解码与消费者分离"的既定
 * 架构原则。字段清单/换算公式/省略规则见
 * docs/superpowers/specs/2026-07-07-m4-json-payload-design.md。
 * 依赖边界：只依赖 state/state_store.hpp 和 nlohmann/json，不包含 uart/、
 * protocol/、config/、mqtt/ 等其他模块头文件。
 */

#include <string>

#include <nlohmann/json.hpp>

#include "cellular/cellular_snapshot.hpp"
#include "state/state_store.hpp"

namespace payload {

/**
 * @brief 把一份遥测快照转成 JSON。
 * @param state 当前的完整遥测快照（state::StateStore::Snapshot() 的返回值）。
 * @param school_name 本机静态配置的学校名称（来自 config.json，不是 STM32 解码出来的）。
 * @return 按设计文档规则组装好的 JSON；未收到过的字段对应的 key 不存在（不输出 null）。
 */
nlohmann::json ToJson(const state::TelemetryState& state, const std::string& school_name);

/**
 * @brief 在既有遥测 JSON 中加入树莓派 5G 链路公开状态。
 */
nlohmann::json ToJson(const state::TelemetryState& state, const std::string& school_name,
                      const cellular::StatusSnapshot& cellular_status);

}  // namespace payload
