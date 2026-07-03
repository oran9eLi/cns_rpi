#pragma once

/**
 * @file mavlink_link.hpp
 * @brief UART 字节流到完整 MAVLink 帧的同步、CRC 校验、编码发送。
 *
 * @details
 * 只负责"凑出一条通过CRC校验的完整帧"和"把一条帧编码发出去"，不解析帧内部字段
 * 语义（那是 M3 protocol/ 层的事）。
 * MavlinkFrameAssembler 是纯状态机，不碰任何 I/O，方便脱离硬件单测；
 * MavlinkLink 组合它和 uart::SerialPort，是真正对外使用的收发入口。
 * 依赖边界：只依赖 uart/serial_port.hpp 和官方 mavlink/c_library_v2 头文件，
 * 不包含 mqtt/、payload/ 等下游模块头文件。
 */

#include <cstdint>
#include <optional>
#include <queue>
#include <span>

#include "common/mavlink.h"
#include "uart/serial_port.hpp"

namespace uart {

/**
 * @brief 把原始字节流喂给官方 mavlink_frame_char_buffer()，攒出完整、CRC校验过的帧。
 * @details 不做任何 I/O，纯状态机；测试直接构造它、喂合成字节序列，不需要真实串口。
 */
class MavlinkFrameAssembler {
 public:
  /**
   * @brief 喂入一批新到达的字节。
   * @details 内部先把 bytes 全部喂给官方解析状态机，凑出的完整帧（含之前调用里还没
   * 取走的）会排进内部队列；然后从队首弹出一条返回。也就是说即使这次调用本身没
   * 凑出新帧，只要队列里还有上次剩下的，也会被返回——调用方不会因为"这次没读到新
   * 字节"就丢失已经解出来但还没被取走的帧。
   * @return 有帧可取就返回它（并从内部队列弹出）；没有则返回 std::nullopt。
   */
  std::optional<mavlink_message_t> Feed(std::span<const std::uint8_t> bytes);

 private:
  mavlink_message_t rx_msg_{};
  mavlink_status_t status_{};
  std::queue<mavlink_message_t> pending_;
};

/// 组合 SerialPort + MavlinkFrameAssembler，是 uart/ 层对外暴露的收发入口。
class MavlinkLink {
 public:
  /**
   * @brief 打开串口并准备好帧同步状态。
   * @param device 字符设备路径，例如 "/dev/ttyUSB0"。
   * @param baud 波特率，参见 uart::SerialPort::Open。
   */
  static std::expected<MavlinkLink, UartError> Open(const std::string& device, int baud);

  MavlinkLink(MavlinkLink&&) noexcept = default;
  MavlinkLink& operator=(MavlinkLink&&) noexcept = default;
  MavlinkLink(const MavlinkLink&) = delete;
  MavlinkLink& operator=(const MavlinkLink&) = delete;

  /**
   * @brief 读取串口当前可用字节并尝试凑出一条完整帧。
   * @details 单次调用最多阻塞 100ms（见 SerialPort::Read 的 VTIME 配置），
   * 不会无限阻塞，调用方可以在循环里穿插做周期性发送。
   * @return 凑出帧就返回；没有则 std::nullopt。串口读失败/读到0字节时本次不会喂入
   * 新字节，但内部 assembler_ 若还有之前调用剩下、尚未取走的帧，仍会被返回——
   * 不能简单认为"读失败就一定返回 std::nullopt"（M2 阶段不做断线重连，那是 M7 的事）。
   */
  std::optional<mavlink_message_t> ReceiveMessage();

  /**
   * @brief 把一条已经 pack 好的帧编码并写入串口。
   * @param message 必须是已经用 mavlink_msg_*_pack 系列函数完成 CRC finalize 的帧。
   * @return 写入成功（字节数与编码长度一致）返回 true，否则 false。
   */
  bool SendMessage(const mavlink_message_t& message);

 private:
  explicit MavlinkLink(SerialPort&& port) : port_(std::move(port)) {}

  SerialPort port_;
  MavlinkFrameAssembler assembler_;
};

}  // namespace uart
