/**
 * @file logger.hpp
 * @brief 提供与业务模块解耦的线程安全日志级别、格式和有界文件输出。
 *
 * @details
 * Logger 由进程启动代码独占创建，可被多个业务线程并发调用；实例通过互斥锁保证
 * 每条记录完整写出。调用方持有返回的 unique_ptr，传入的输出流必须比 Logger 活得久。
 * 本模块不解析配置文件，也不允许业务模块自行拼接时间戳或级别前缀。
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace logging {

class LoggerTestPeer;

/// 日志严重程度；枚举顺序同时定义最低级别过滤规则。
enum class Level { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

/**
 * @brief 把配置中的小写级别名转换为强类型级别。
 * @return 仅接受 debug/info/warn/error；其他输入返回中文错误说明，不抛异常。
 * @note 可在任意线程调用。
 */
std::expected<Level, std::string> ParseLevel(std::string_view text);

/// 日志创建参数，由启动配置适配层填写，容量单位为字节。
struct Options {
  Level minimum_level;
  std::filesystem::path file;
  std::size_t max_file_size_bytes;
};

/**
 * @brief 可注入时钟，用于稳定测试时间格式与进程内单调耗时。
 * @note 函数对象会在 Logger 的互斥区内调用，必须快速返回且不得回调同一 Logger。
 */
struct Clock {
  std::function<std::chrono::system_clock::time_point()> system_now;
  std::function<std::chrono::steady_clock::time_point()> steady_now;
};

/**
 * @brief 进程级线程安全日志器，把低级别输出到 stdout、高级别输出到 stderr。
 *
 * Logger 不拥有输出流；调用方必须保证流的生命周期覆盖 Logger，且不得绕过 Logger
 * 从其他线程并发写同一输出流。
 */
class Logger {
 public:
  /**
   * @brief 创建日志器并记录单调时钟起点。
   * @return 成功返回独占实例；创建失败返回原因且不抛异常。
   * @note 创建后可把实例共享给任意业务线程，但输出流仍由调用方管理。
   */
  static std::expected<std::unique_ptr<Logger>, std::string> Create(
      Options options, std::ostream& out, std::ostream& err, Clock clock = {});

  /// 判断指定级别当前是否会输出；可在任意线程调用。
  bool Enabled(Level level) const;

  /// 输出调试记录；可在任意线程调用，消息中的 CR/LF 会被替换为空格。
  void Debug(std::string_view message);
  /// 输出信息记录；可在任意线程调用，消息中的 CR/LF 会被替换为空格。
  void Info(std::string_view message);
  /// 输出警告记录；可在任意线程调用，消息中的 CR/LF 会被替换为空格。
  void Warn(std::string_view message);
  /// 输出错误记录；可在任意线程调用，消息中的 CR/LF 会被替换为空格。
  void Error(std::string_view message);

 private:
  enum class AppendResult { kNotWritten, kWritten, kVisibleButNotDurable };
  struct FileOperations {
    int (*sync)(int);
    int (*replace)(const char*, const char*);
    ssize_t (*write)(int, const void*, std::size_t);
    int (*close)(int);
    int (*truncate)(int, off_t);
  };

  friend class LoggerTestPeer;
  Logger(Options options, std::ostream& out, std::ostream& err, Clock clock,
         std::chrono::steady_clock::time_point start);
  void Write(Level level, std::string_view message);
  std::expected<void, std::string> InitializeFile();
  AppendResult AppendOrCompact(std::string line);
  AppendResult CompactAndAppend(std::string_view line);
  void FallBackToStandardError(std::string_view reason);
  static void SetFileOperationsForTesting(const FileOperations* operations);
  static const FileOperations* file_operations_;

  Options options_;
  std::ostream& out_;
  std::ostream& err_;
  Clock clock_;
  std::chrono::steady_clock::time_point start_;
  bool file_output_enabled_ = false;
  bool fallback_reported_ = false;
  mutable std::mutex mutex_;
};

}  // namespace logging
