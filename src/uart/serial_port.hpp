#pragma once

/**
 * @file serial_port.hpp
 * @brief 串口字节收发的 termios 封装。
 *
 * @details
 * 只负责"打开一个字符设备路径、配置波特率/8N1/raw模式、阻塞读写字节"，
 * 不知道 MAVLink 是什么，不做任何帧同步。帧同步在 uart/mavlink_link.hpp 里。
 * 依赖边界：只依赖 POSIX termios，不包含 mavlink/、mqtt/ 等上层模块头文件。
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace uart {

/// 串口相关操作的失败原因。
enum class UartError {
  kDeviceNotFound,    ///< 设备路径不存在
  kPermissionDenied,  ///< 没有读写权限（比如不在 dialout 组）
  kConfigFailed,      ///< termios 配置失败，或波特率不在支持列表里
  kReadError,         ///< read() 系统调用失败
  kWriteError,        ///< write() 系统调用失败
};

/// 一个已打开、已配置好的串口。只能移动，不能拷贝（持有一个 fd）。
class SerialPort {
 public:
  /**
   * @brief 打开并配置一个串口。
   * @param device 字符设备路径，例如 "/dev/ttyUSB0"。
   * @param baud 波特率，支持 9600/19200/38400/57600/115200/230400，其余值返回 kConfigFailed。
   * @return 成功返回可用的 SerialPort；失败返回具体错误原因。
   */
  static std::expected<SerialPort, UartError> Open(const std::string& device, int baud);

  SerialPort(SerialPort&& other) noexcept;
  SerialPort& operator=(SerialPort&& other) noexcept;
  SerialPort(const SerialPort&) = delete;
  SerialPort& operator=(const SerialPort&) = delete;
  ~SerialPort();

  /**
   * @brief 读取当前可用的字节，最多填满 buffer。
   * @details 配置为 VMIN=0/VTIME=1（100ms），没有数据时最多阻塞 100ms 后返回 0，
   * 不会无限阻塞——这样调用方（uart::MavlinkLink 的轮询循环）能穿插做周期性发送，
   * 不必开线程。
   * @return 实际读到的字节数（可能是 0）；系统调用失败返回 kReadError。
   */
  std::expected<std::size_t, UartError> Read(std::span<std::uint8_t> buffer);

  /**
   * @brief 把 data 全部写入串口，内部处理短写（循环写完为止）。
   * @return 成功返回写入的字节数（等于 data.size()）；失败返回 kWriteError。
   */
  std::expected<std::size_t, UartError> Write(std::span<const std::uint8_t> data);

 private:
  explicit SerialPort(int fd) : fd_(fd) {}

  int fd_ = -1;
};

}  // namespace uart
