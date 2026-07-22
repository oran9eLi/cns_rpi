/**
 * @file logger.cpp
 * @brief 实现日志级别过滤、固定宽度时间前缀和有界单文件输出。
 *
 * @details
 * 系统时间按设备本地时区格式化，异常或明显未校时时使用固定占位符。文件压缩替换遵循
 * POSIX 同目录原子替换和 fsync 持久化边界，不生成历史文件，也不依赖 mqtt/ 或 protocol/。
 */

#include "logging/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace logging {
namespace {

constexpr std::string_view kInvalidTimestamp = "---------- --:--:--.---";

std::string_view LevelName(Level level) {
  switch (level) {
    case Level::kDebug:
      return "DEBUG";
    case Level::kInfo:
      return "INFO ";
    case Level::kWarn:
      return "WARN ";
    case Level::kError:
      return "ERROR";
  }
  return "ERROR";
}

std::string FormatSystemTime(std::chrono::system_clock::time_point time) {
  using namespace std::chrono;
  constexpr auto kEarliestValid = sys_days{year{2025} / 1 / 1};
  if (time < kEarliestValid) {
    return std::string(kInvalidTimestamp);
  }

  const auto whole_seconds = floor<seconds>(time);
  const auto milliseconds = duration_cast<std::chrono::milliseconds>(time - whole_seconds).count();
  const std::time_t raw_time = std::chrono::system_clock::to_time_t(whole_seconds);
  std::tm local_time{};
  if (localtime_r(&raw_time, &local_time) == nullptr) {
    return std::string(kInvalidTimestamp);
  }

  std::ostringstream formatted;
  formatted << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0')
            << std::setw(3) << milliseconds;
  if (!formatted) {
    return std::string(kInvalidTimestamp);
  }
  return formatted.str();
}

std::string FormatElapsed(std::chrono::steady_clock::duration elapsed) {
  constexpr std::int64_t kMaximumDisplayMilliseconds = 999'999'999;
  const auto elapsed_ms = std::clamp<std::int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 0,
      kMaximumDisplayMilliseconds);
  const auto seconds = elapsed_ms / 1000;
  const auto milliseconds = elapsed_ms % 1000;
  std::ostringstream formatted;
  formatted << '+' << std::setfill('0') << std::setw(6) << seconds << '.' << std::setw(3)
            << milliseconds << 's';
  return formatted.str();
}

std::string SingleLine(std::string_view message) {
  std::string result(message);
  std::replace_if(result.begin(), result.end(),
                  [](char value) { return value == '\r' || value == '\n'; }, ' ');
  return result;
}

void TruncateRecord(std::string& line, std::size_t maximum) {
  constexpr std::string_view kTruncated = "[已截断]\n";
  if (line.size() <= maximum) return;
  if (maximum <= kTruncated.size()) {
    line.resize(maximum);
    if (!line.empty()) line.back() = '\n';
    return;
  }
  line.resize(maximum - kTruncated.size());
  line.append(kTruncated);
}

struct WriteResult {
  bool complete;
  std::size_t bytes_written;
};

WriteResult WriteFully(int descriptor, std::string_view text,
                       ssize_t (*write_operation)(int, const void*, std::size_t)) {
  std::size_t bytes_written = 0;
  while (!text.empty()) {
    const ssize_t written = write_operation(descriptor, text.data(), text.size());
    if (written < 0) {
      if (errno == EINTR) continue;
      return {.complete = false, .bytes_written = bytes_written};
    }
    if (written == 0) return {.complete = false, .bytes_written = bytes_written};
    bytes_written += static_cast<std::size_t>(written);
    text.remove_prefix(static_cast<std::size_t>(written));
  }
  return {.complete = true, .bytes_written = bytes_written};
}

std::string ErrnoMessage(std::string_view operation) {
  return std::string(operation) + "失败: " + std::strerror(errno);
}

}  // namespace

const Logger::FileOperations* Logger::file_operations_ = nullptr;

void Logger::SetFileOperationsForTesting(const FileOperations* operations) {
  file_operations_ = operations;
}

std::expected<Level, std::string> ParseLevel(std::string_view text) {
  if (text == "debug") return Level::kDebug;
  if (text == "info") return Level::kInfo;
  if (text == "warn") return Level::kWarn;
  if (text == "error") return Level::kError;
  return std::unexpected("日志级别必须是debug、info、warn或error");
}

Logger::Logger(Options options, std::ostream& out, std::ostream& err, Clock clock,
               std::chrono::steady_clock::time_point start)
    : options_(std::move(options)), out_(out), err_(err), clock_(std::move(clock)), start_(start) {}

std::expected<std::unique_ptr<Logger>, std::string> Logger::Create(
    Options options, std::ostream& out, std::ostream& err, Clock clock) {
  if (!clock.system_now) {
    clock.system_now = [] { return std::chrono::system_clock::now(); };
  }
  if (!clock.steady_now) {
    clock.steady_now = [] { return std::chrono::steady_clock::now(); };
  }
  const auto start = clock.steady_now();
  auto logger = std::unique_ptr<Logger>(
      new Logger(std::move(options), out, err, std::move(clock), start));
  if (auto initialized = logger->InitializeFile(); !initialized) {
    return std::unexpected(initialized.error());
  }
  return logger;
}

bool Logger::Enabled(Level level) const {
  return static_cast<int>(level) >= static_cast<int>(options_.minimum_level);
}

void Logger::Debug(std::string_view message) { Write(Level::kDebug, message); }
void Logger::Info(std::string_view message) { Write(Level::kInfo, message); }
void Logger::Warn(std::string_view message) { Write(Level::kWarn, message); }
void Logger::Error(std::string_view message) { Write(Level::kError, message); }

void Logger::Write(Level level, std::string_view message) {
  std::lock_guard lock(mutex_);
  if (!Enabled(level)) {
    return;
  }

  std::ostringstream formatted;
  formatted << '[' << FormatSystemTime(clock_.system_now()) << "] ["
            << FormatElapsed(clock_.steady_now() - start_) << "] [" << LevelName(level) << "] "
            << SingleLine(message) << '\n';
  std::string line = std::move(formatted).str();
  if (file_output_enabled_) {
    const auto result = AppendOrCompact(line);
    if (result == AppendResult::kWritten) return;
    if (result == AppendResult::kVisibleButNotDurable) {
      FallBackToStandardError("日志文件持久化确认失败，持久化状态不确定");
      return;
    }
    FallBackToStandardError("日志文件运行时写入失败");
  }
  std::ostream& destination = fallback_reported_ ? err_ : (level < Level::kWarn ? out_ : err_);
  destination << line << std::flush;
}

std::expected<void, std::string> Logger::InitializeFile() {
  if (options_.file.empty()) return {};
  if (options_.max_file_size_bytes == 0) {
    return std::unexpected("日志文件容量上限必须大于0");
  }
  const auto parent = options_.file.parent_path().empty() ? std::filesystem::path(".")
                                                          : options_.file.parent_path();
  std::error_code error;
  if (!std::filesystem::is_directory(parent, error)) {
    return std::unexpected("日志文件父目录不存在: " + parent.string());
  }
  if (std::filesystem::is_directory(options_.file, error)) {
    return std::unexpected("日志文件目标不能是目录: " + options_.file.string());
  }
  const int descriptor = open(options_.file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                              0644);
  if (descriptor < 0) return std::unexpected(ErrnoMessage("打开日志文件"));
  if (close(descriptor) != 0) return std::unexpected(ErrnoMessage("关闭日志文件"));
  file_output_enabled_ = true;
  const auto size = std::filesystem::file_size(options_.file, error);
  if (error) return std::unexpected("读取日志文件大小失败: " + error.message());
  if (size > options_.max_file_size_bytes) {
    const auto result = CompactAndAppend({});
    if (result == AppendResult::kWritten) return {};
    file_output_enabled_ = false;
    if (result == AppendResult::kVisibleButNotDurable) {
      return std::unexpected("日志文件持久化状态不确定");
    }
    return std::unexpected("收敛已有日志文件失败");
  }
  return {};
}

Logger::AppendResult Logger::AppendOrCompact(std::string line) {
  const std::size_t maximum = options_.max_file_size_bytes;
  TruncateRecord(line, maximum);
  std::error_code error;
  const auto old_size = std::filesystem::file_size(options_.file, error);
  if (error) return AppendResult::kNotWritten;
  if (old_size + line.size() > maximum) return CompactAndAppend(line);

  const int descriptor = open(options_.file.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
  if (descriptor < 0) return AppendResult::kNotWritten;
  const auto write_operation = file_operations_ ? file_operations_->write : write;
  const auto close_operation = file_operations_ ? file_operations_->close : close;
  const auto truncate_operation = file_operations_ ? file_operations_->truncate : ftruncate;
  const WriteResult result = WriteFully(descriptor, line, write_operation);
  if (result.complete) {
    return close_operation(descriptor) == 0 ? AppendResult::kWritten
                                             : AppendResult::kVisibleButNotDurable;
  }
  if (result.bytes_written == 0) {
    (void)close_operation(descriptor);
    return AppendResult::kNotWritten;
  }

  bool rolled_back = truncate_operation(descriptor, static_cast<off_t>(old_size)) == 0;
  struct stat status {};
  rolled_back = rolled_back && fstat(descriptor, &status) == 0 &&
                static_cast<std::uintmax_t>(status.st_size) == old_size;
  if (!rolled_back && result.bytes_written > 0) {
    // 回滚失败时尽力封闭残留记录，避免后续降级日志被误认为同一行的一部分。
    const char newline = '\n';
    (void)write(descriptor, &newline, 1);
    (void)fsync(descriptor);
  }
  (void)close_operation(descriptor);
  return rolled_back ? AppendResult::kNotWritten : AppendResult::kVisibleButNotDurable;
}

Logger::AppendResult Logger::CompactAndAppend(std::string_view input_line) {
  std::string line(input_line);
  const std::size_t maximum = options_.max_file_size_bytes;
  TruncateRecord(line, maximum);
  std::ifstream input(options_.file, std::ios::binary);
  if (!input) return AppendResult::kNotWritten;
  std::string old{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad()) return AppendResult::kNotWritten;

  const std::size_t required_drop =
      old.size() + line.size() > maximum ? old.size() + line.size() - maximum : 0;
  std::size_t keep_from = std::max(required_drop, std::min(old.size(), maximum / 4));
  if (keep_from < old.size()) {
    const auto newline = old.find('\n', keep_from);
    keep_from = newline == std::string::npos ? old.size() : newline + 1;
  }
  const std::string replacement = old.substr(keep_from) + line;

  std::string temporary = options_.file.string() + ".tmp.XXXXXX";
  std::vector<char> name(temporary.begin(), temporary.end());
  name.push_back('\0');
  const int descriptor = mkstemp(name.data());
  if (descriptor < 0) return AppendResult::kNotWritten;
  const std::filesystem::path temporary_path(name.data());
  struct stat original_status {};
  const mode_t replacement_mode =
      stat(options_.file.c_str(), &original_status) == 0 ? original_status.st_mode & 0777 : 0644;
  const auto sync_file = file_operations_ ? file_operations_->sync : fsync;
  const auto replace_file = file_operations_ ? file_operations_->replace : rename;
  const auto write_file = file_operations_ ? file_operations_->write : write;
  const auto close_file = file_operations_ ? file_operations_->close : close;
  const WriteResult write_result = WriteFully(descriptor, replacement, write_file);
  bool succeeded = fchmod(descriptor, replacement_mode) == 0 && write_result.complete &&
                   sync_file(descriptor) == 0;
  if (close_file(descriptor) != 0) succeeded = false;
  bool replaced = false;
  if (succeeded && replace_file(temporary_path.c_str(), options_.file.c_str()) != 0) {
    succeeded = false;
  } else if (succeeded) {
    replaced = true;
  }
  if (succeeded) {
    const auto parent = options_.file.parent_path().empty() ? std::filesystem::path(".")
                                                            : options_.file.parent_path();
    const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    succeeded = directory >= 0;
    if (directory >= 0) {
      const bool directory_synced = sync_file(directory) == 0;
      const bool directory_closed = close_file(directory) == 0;
      if (!directory_synced || !directory_closed) succeeded = false;
    }
  }
  if (!succeeded && !replaced) {
    std::error_code ignored;
    std::filesystem::remove(temporary_path, ignored);
  }
  if (succeeded) return AppendResult::kWritten;
  return replaced ? AppendResult::kVisibleButNotDurable : AppendResult::kNotWritten;
}

void Logger::FallBackToStandardError(std::string_view reason) {
  file_output_enabled_ = false;
  if (fallback_reported_) return;
  fallback_reported_ = true;
  err_ << "[日志] " << reason << "，已降级到stderr\n";
}

}  // namespace logging
