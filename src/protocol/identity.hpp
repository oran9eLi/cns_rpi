#pragma once

/**
 * @file identity.hpp
 * @brief 跟 MAVLink 消息 payload 内容无关的身份数据处理：DCDW 角色号格式化、
 * uas_id 字节数组转字符串、uas_id 合法性校验。
 *
 * @details
 * `OPEN_DRONE_ID_*` 消息本身的解码在 extension_decoder.hpp/.cpp 里(跟
 * NAMED_VALUE_INT/TUNNEL 同一个文件，同一个 DecodeExtensionAndStore 函数)。
 * 这个文件只处理更底层的两件事：帧头 sysid 格式化(不是 payload 字段)、
 * uas_id 字节数组的提取与校验(供 main.cpp 调用，因为它跟"身份"这个概念强相关)。
 * 依赖边界：只依赖标准库和 device/product_info.hpp，不包含 state/、uart/ 等模块头文件。
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "device/product_info.hpp"

namespace protocol {

/// 主控箱厂商唯一产品识别码的固定前缀：厂商码 DCDW + 产品型号码 CNS1。
constexpr std::string_view kCnsBoxManufacturerCode = "DCDW";
constexpr std::string_view kCnsBoxModelCode = "CNS1";

/**
 * @brief 从 MAVLink 帧头 sysid 格式化 DCDW 角色号，3 位数字补零。
 * @param sysid MAVLink 帧头的 system_id 字段(uint8_t，最大 255，3 位数字够用)。
 * @return 形如 "DCDW-007" 的字符串。
 * @details 依据 docs/设备标识符.md §3：这个数字就是固件的 PX4LITE_UNIT_ID，
 * 也是 MAVLink 帧头的 system_id，不是某个消息 payload 里的字段。
 */
std::string FormatDcdwLabel(std::uint8_t sysid);

/**
 * @brief 从 uas_id(20 字节，未用部分填 null)提取受控设备身份字符串。
 * @param uas_id 对应 mavlink_open_drone_id_basic_id_t::uas_id 的原始字段
 * (uint8_t[20]，C 数组，按引用传递保留长度信息，调用点直接传 value.uas_id)。
 * @return 用 strnlen 求实际长度后转成的字符串，本函数不做合法性判断——
 * 校验交给 IsValidUasId()，这样调用点能把"取到了什么"写进日志。
 * 20 字节写满、无 null 终止符是合法输入，此时返回整 20 字节转成的字符串。
 */
std::string ExtractUasId(const std::uint8_t (&uas_id)[20]);

/**
 * @brief 校验 uas_id 能否直接当作 device_id 用于 MQTT topic 与 Client ID。
 *
 * 见设计文档 §3.3：主控箱和 PX4 共用这一套校验，不区分设备类型。字符集限制在
 * ASCII 字母数字加 `-_.:`，因此 MQTT 通配符 `+`/`#` 和层级分隔符 `/` 天然被拒。
 * 长度上限 20 字节来自 MAVLink 字段宽度本身。
 */
bool IsValidUasId(const std::string& uas_id);

/**
 * @brief 判断 device_id 是否带主控箱的 DCDWCNS1 产品前缀。
 *
 * 只用于尽早发现固件身份配置错误并告警，不参与 device_type 判断，
 * 也不作为拒绝建立 MQTT 会话的理由(设计文档 §3.3)。
 */
bool HasCnsBoxProductPrefix(const std::string& device_id);

/**
 * @brief 从主控箱 device_id 的前 8 字节拆出产品信息。
 * @return 前缀匹配时返回 {DCDW, CNS1}；不匹配返回 nullopt，不猜测、不补默认值。
 * @details 依据设计文档 §4.2：主控箱 device_id 的结构是
 * `厂商码(4) | 产品型号码(4) | 唯一序列(12)`，产品信息本来就编码在身份里，
 * 不需要固件再单独发一条消息。
 */
std::optional<device::ProductInfo> CnsBoxProductFrom(const std::string& device_id);

}  // namespace protocol
