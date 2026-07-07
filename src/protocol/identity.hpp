#pragma once

/**
 * @file identity.hpp
 * @brief M3c 范围内跟 MAVLink 消息 payload 内容无关的身份数据处理：
 * DCDW 角色号格式化、RPi 硬件序列号读取、uas_id 字节数组转字符串。
 *
 * @details
 * `OPEN_DRONE_ID_*` 消息本身的解码在 extension_decoder.hpp/.cpp 里(跟
 * NAMED_VALUE_INT/TUNNEL 同一个文件，同一个 DecodeExtensionAndStore 函数)。
 * 这个文件只处理三件更底层的事：帧头 sysid 格式化(不是 payload 字段)、
 * 本机文件读取(跟 MAVLink 帧完全无关)、uas_id 字节数组转字符串(供
 * extension_decoder.cpp 调用，因为提取逻辑跟"身份"这个概念强相关)。
 * 依赖边界：只依赖标准库，不包含 state/、uart/ 等模块头文件。
 */

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace protocol {

/**
 * @brief 从 MAVLink 帧头 sysid 格式化 DCDW 角色号，3 位数字补零。
 * @param sysid MAVLink 帧头的 system_id 字段(uint8_t，最大 255，3 位数字够用)。
 * @return 形如 "DCDW-007" 的字符串。
 * @details 依据 docs/设备标识符.md §3：这个数字就是固件的 PX4LITE_UNIT_ID，
 * 也是 MAVLink 帧头的 system_id，不是某个消息 payload 里的字段。
 */
std::string FormatDcdwLabel(std::uint8_t sysid);

/**
 * @brief 读取 RPi 本机硬件序列号(/proc/cpuinfo 的 Serial 行)。
 * @param path 【测试专用】仅供单元测试注入 fixture 文件(tests/test_identity.cpp)，
 * 真机代码(main.cpp)从不显式传参，永远用默认值读真实 /proc/cpuinfo。
 * M3c 真机验证通过后，评估是否删掉这个参数(连同依赖它的 fixture 测试一起去掉)
 * 或注释掉，不作为长期对外接口保留。
 * @return 找到 Serial 行则返回其值(去掉前后空白)；文件不存在或没有 Serial 行
 * 则返回 std::nullopt——V1 过渡期字段，读不到不是错误，只是没有这个信息。
 */
std::optional<std::string> ReadRpiSerial(const std::filesystem::path& path = "/proc/cpuinfo");

/**
 * @brief 从 uas_id(20 字节，未用部分填 null)提取厂商唯一产品识别码字符串。
 * @param uas_id 对应 mavlink_open_drone_id_basic_id_t::uas_id 的原始字段
 * (uint8_t[20]，C 数组，按引用传递保留长度信息，调用点直接传 value.uas_id)。
 * @return 用 strnlen 求实际长度后转成的字符串，不做格式校验(RPi 不校验身份数据，
 * 见 docs/设备标识符.md §2.3)。20 字节写满、无 null 终止符是合法输入，
 * 此时返回整 20 字节转成的字符串，不是错误。
 */
std::string ExtractVendorId(const std::uint8_t (&uas_id)[20]);

}  // namespace protocol
