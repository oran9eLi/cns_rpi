#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "logging/logger.hpp"

namespace logging {

class LoggerTestPeer {
 public:
  static void FailDirectorySync() {
    operations_ = {.sync = [](int descriptor) {
                     struct stat status {};
                     if (fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode)) {
                       errno = EIO;
                       return -1;
                     }
                     return fsync(descriptor);
                   },
                   .replace = rename,
                   .write = write,
                   .close = close,
                   .truncate = ftruncate};
    Logger::SetFileOperationsForTesting(&operations_);
  }

  static void FailRename() {
    operations_ = {.sync = fsync,
                   .replace = [](const char*, const char*) {
                     errno = EIO;
                     return -1;
                   },
                   .write = write,
                   .close = close,
                   .truncate = ftruncate};
    Logger::SetFileOperationsForTesting(&operations_);
  }

  static void FailAfterPartialWrite() {
    partial_write_calls_ = 0;
    operations_ = DefaultOperations();
    operations_.write = [](int descriptor, const void* data, std::size_t size) -> ssize_t {
      if (partial_write_calls_++ == 0) {
        return write(descriptor, data, size > 8 ? size - 8 : size / 2);
      }
      errno = EIO;
      return -1;
    };
    Logger::SetFileOperationsForTesting(&operations_);
  }

  static void FailCloseAfterWriting() {
    operations_ = DefaultOperations();
    operations_.close = [](int descriptor) {
      const int result = close(descriptor);
      errno = EIO;
      return result == 0 ? -1 : result;
    };
    Logger::SetFileOperationsForTesting(&operations_);
  }

  static void FailPartialWriteAndRollback() {
    FailAfterPartialWrite();
    operations_.truncate = [](int, off_t) {
      errno = EIO;
      return -1;
    };
  }

  static void FailFirstWriteAndTruncate() {
    operations_ = DefaultOperations();
    operations_.write = [](int, const void*, std::size_t) -> ssize_t {
      errno = EIO;
      return -1;
    };
    operations_.truncate = [](int, off_t) {
      errno = EIO;
      return -1;
    };
    Logger::SetFileOperationsForTesting(&operations_);
  }

  static void Reset() { Logger::SetFileOperationsForTesting(nullptr); }

 private:
  static Logger::FileOperations DefaultOperations() {
    return {.sync = fsync,
            .replace = rename,
            .write = write,
            .close = close,
            .truncate = ftruncate};
  }
  static inline int partial_write_calls_ = 0;
  static inline Logger::FileOperations operations_ = DefaultOperations();
};

}  // namespace logging

namespace {

using namespace std::chrono_literals;

logging::Options ConsoleOptions(logging::Level level) {
  return {.minimum_level = level, .file = {}, .max_file_size_bytes = 1024U};
}

class TempDirectory {
 public:
  TempDirectory() {
    std::string pattern = (std::filesystem::temp_directory_path() / "cns-logger-XXXXXX").string();
    storage_.assign(pattern.begin(), pattern.end());
    storage_.push_back('\0');
    REQUIRE(mkdtemp(storage_.data()) != nullptr);
    path_ = storage_.data();
  }

  ~TempDirectory() { std::filesystem::remove_all(path_); }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::vector<char> storage_;
  std::filesystem::path path_;
};

class ScopedFileFault {
 public:
  explicit ScopedFileFault(void (*enable)()) { enable(); }
  ~ScopedFileFault() { logging::LoggerTestPeer::Reset(); }
};

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

logging::Options FileOptions(const std::filesystem::path& file, std::size_t maximum = 1024U) {
  return {.minimum_level = logging::Level::kDebug,
          .file = file,
          .max_file_size_bytes = maximum};
}

class ScopedTimezone {
 public:
  explicit ScopedTimezone(const char* timezone) {
    if (const char* previous = std::getenv("TZ")) {
      previous_ = previous;
    }
    REQUIRE(setenv("TZ", timezone, 1) == 0);
    tzset();
  }

  ~ScopedTimezone() {
    if (previous_) {
      setenv("TZ", previous_->c_str(), 1);
    } else {
      unsetenv("TZ");
    }
    tzset();
  }

  ScopedTimezone(const ScopedTimezone&) = delete;
  ScopedTimezone& operator=(const ScopedTimezone&) = delete;

 private:
  std::optional<std::string> previous_;
};

class SyncCountingBuffer : public std::stringbuf {
 public:
  int sync() override {
    ++sync_count_;
    return std::stringbuf::sync();
  }

  int sync_count() const { return sync_count_; }

 private:
  int sync_count_ = 0;
};

TEST_CASE("控制台每条日志立即刷新到systemd管道") {
  SyncCountingBuffer out_buffer;
  SyncCountingBuffer err_buffer;
  std::ostream out(&out_buffer);
  std::ostream err(&err_buffer);
  auto logger = logging::Logger::Create(ConsoleOptions(logging::Level::kInfo), out, err);
  REQUIRE(logger.has_value());

  (*logger)->Info("启动");
  (*logger)->Warn("可恢复失败");

  CHECK(out_buffer.sync_count() == 1);
  CHECK(err_buffer.sync_count() == 1);
}

TEST_CASE("控制台按级别过滤、分流并清理换行") {
  std::ostringstream out;
  std::ostringstream err;
  const auto invalid_system_time = std::chrono::system_clock::time_point{};
  const auto steady_start = std::chrono::steady_clock::time_point{};
  logging::Clock clock{
      .system_now = [invalid_system_time] { return invalid_system_time; },
      .steady_now = [steady_start] { return steady_start; },
  };

  CHECK(logging::ParseLevel("debug") == logging::Level::kDebug);
  CHECK_FALSE(logging::ParseLevel("INFO").has_value());
  auto logger = logging::Logger::Create(ConsoleOptions(logging::Level::kInfo), out, err, clock);
  REQUIRE(logger.has_value());

  (*logger)->Debug("不可见");
  (*logger)->Info("已连接");
  (*logger)->Warn("第一行\n第二行");

  CHECK(out.str() == "[---------- --:--:--.---] [+000000.000s] [INFO ] 已连接\n");
  CHECK(err.str() == "[---------- --:--:--.---] [+000000.000s] [WARN ] 第一行 第二行\n");
}

TEST_CASE("系统时间按本地非UTC时区输出") {
  const ScopedTimezone timezone("UTC-8");
  std::ostringstream out;
  std::ostringstream err;
  const auto system_time = std::chrono::sys_days{std::chrono::year{2026} / 7 / 18} +
                           6h + 10min + 23s + 125ms;
  const auto steady_start = std::chrono::steady_clock::time_point{};
  logging::Clock clock{
      .system_now = [system_time] { return system_time; },
      .steady_now = [steady_start] { return steady_start; },
  };
  auto logger = logging::Logger::Create(ConsoleOptions(logging::Level::kInfo), out, err, clock);
  REQUIRE(logger.has_value());

  (*logger)->Info("本地时间");

  CHECK(out.str().starts_with("[2026-07-18 14:10:23.125]"));
}

TEST_CASE("长时间运行后单调时间仍保持固定宽度") {
  std::ostringstream out;
  std::ostringstream err;
  const auto system_time = std::chrono::sys_days{std::chrono::year{2026} / 7 / 18};
  int steady_calls = 0;
  logging::Clock clock{
      .system_now = [system_time] { return system_time; },
      .steady_now = [&steady_calls] {
        return std::chrono::steady_clock::time_point{
            steady_calls++ == 0 ? 0s : 1'000'000s};
      },
  };
  auto logger = logging::Logger::Create(ConsoleOptions(logging::Level::kInfo), out, err, clock);
  REQUIRE(logger.has_value());

  (*logger)->Info("仍为定宽");

  CHECK(out.str().find("[+999999.999s]") != std::string::npos);
}

TEST_CASE("文件模式只写文件且拒绝目录目标") {
  TempDirectory temp;
  std::ostringstream out;
  std::ostringstream err;
  const auto file = temp.path() / "node.log";
  auto logger = logging::Logger::Create(FileOptions(file), out, err);
  REQUIRE(logger.has_value());

  (*logger)->Info("只进文件");

  CHECK(ReadText(file).find("只进文件") != std::string::npos);
  CHECK(out.str().empty());
  CHECK(err.str().empty());
  CHECK_FALSE(logging::Logger::Create(FileOptions(temp.path()), out, err).has_value());
}

TEST_CASE("单文件滚动保留完整的新记录且不生成历史文件") {
  TempDirectory temp;
  std::ostringstream out;
  std::ostringstream err;
  const auto file = temp.path() / "node.log";
  auto logger = logging::Logger::Create(FileOptions(file), out, err);
  REQUIRE(logger.has_value());
  REQUIRE(chmod(file.c_str(), 0640) == 0);

  for (int index = 0; index < 100; ++index) {
    std::ostringstream message;
    message << "record-" << std::setfill('0') << std::setw(4) << index;
    (*logger)->Info(message.str());
  }

  const auto text = ReadText(file);
  CHECK(std::filesystem::file_size(file) <= 1024);
  CHECK(text.find("record-0000") == std::string::npos);
  CHECK(text.find("record-0099") != std::string::npos);
  REQUIRE_FALSE(text.empty());
  CHECK(text.front() == '[');
  struct stat status {};
  REQUIRE(stat(file.c_str(), &status) == 0);
  CHECK((status.st_mode & 0777) == 0640);
  CHECK(std::distance(std::filesystem::directory_iterator(temp.path()),
                      std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("启动时收敛已有超限文件并截断超大单条") {
  TempDirectory temp;
  std::ostringstream out;
  std::ostringstream err;
  const auto file = temp.path() / "node.log";
  {
    std::ofstream seed(file);
    for (int index = 0; index < 80; ++index) seed << "[old] record-" << index << '\n';
  }
  auto logger = logging::Logger::Create(FileOptions(file), out, err);
  REQUIRE(logger.has_value());
  CHECK(std::filesystem::file_size(file) <= 1024);

  (*logger)->Info("BEGIN-" + std::string(4096, 'X') + "-END");

  const auto text = ReadText(file);
  CHECK(std::filesystem::file_size(file) <= 1024);
  CHECK(text.front() == '[');
  CHECK(text.back() == '\n');
  CHECK(text.find("BEGIN-") != std::string::npos);
  CHECK(text.find("-END") == std::string::npos);
  CHECK(text.find("[已截断]") != std::string::npos);
}

TEST_CASE("启动收敛替换成功但目录同步失败拒绝创建Logger") {
  TempDirectory temp;
  std::ostringstream out;
  std::ostringstream err;
  const auto file = temp.path() / "node.log";
  {
    std::ofstream seed(file);
    for (int index = 0; index < 80; ++index) seed << "[old] record-" << index << '\n';
  }
  const ScopedFileFault fault(logging::LoggerTestPeer::FailDirectorySync);

  const auto logger = logging::Logger::Create(FileOptions(file), out, err);

  REQUIRE_FALSE(logger.has_value());
  CHECK(logger.error().find("日志文件持久化状态不确定") != std::string::npos);
}

TEST_CASE("并发文件记录不交错且容量受限") {
  TempDirectory temp;
  std::ostringstream out;
  std::ostringstream err;
  const auto file = temp.path() / "node.log";
  auto logger = logging::Logger::Create(FileOptions(file, 64 * 1024), out, err);
  REQUIRE(logger.has_value());
  std::vector<std::thread> threads;
  for (int thread = 0; thread < 4; ++thread) {
    threads.emplace_back([&, thread] {
      for (int index = 0; index < 100; ++index) {
        (*logger)->Info("thread-" + std::to_string(thread) + "-record-" +
                       std::to_string(index));
      }
    });
  }
  for (auto& thread : threads) thread.join();

  std::vector<int> occurrences(400);
  std::istringstream lines(ReadText(file));
  for (std::string line; std::getline(lines, line);) {
    CHECK(line.starts_with("["));
    const auto message = line.find("] [INFO ] thread-");
    REQUIRE(message != std::string::npos);
    int thread = -1;
    int record = -1;
    CHECK(std::sscanf(line.c_str() + message, "] [INFO ] thread-%d-record-%d", &thread,
                      &record) == 2);
    REQUIRE(thread >= 0);
    REQUIRE(thread < 4);
    REQUIRE(record >= 0);
    REQUIRE(record < 100);
    ++occurrences[thread * 100 + record];
  }
  CHECK(std::all_of(occurrences.begin(), occurrences.end(), [](int count) { return count == 1; }));
  CHECK(std::filesystem::file_size(file) <= 64 * 1024);
}

TEST_CASE("替换成功但目录同步失败不重复输出业务记录") {
  TempDirectory temp;
  std::ostringstream out;
  std::ostringstream err;
  const auto file = temp.path() / "node.log";
  auto logger = logging::Logger::Create(FileOptions(file, 256), out, err);
  REQUIRE(logger.has_value());
  (*logger)->Info("填充记录一");
  (*logger)->Info("填充记录二");
  (*logger)->Info("填充记录三");
  const ScopedFileFault fault(logging::LoggerTestPeer::FailDirectorySync);

  (*logger)->Info("唯一业务记录");
  (*logger)->Info("后续降级记录");

  const auto file_text = ReadText(file);
  const auto errors = err.str();
  const auto occurrences = (file_text.find("唯一业务记录") != std::string::npos ? 1 : 0) +
                           (errors.find("唯一业务记录") != std::string::npos ? 1 : 0);
  CHECK(occurrences == 1);
  CHECK(errors.find("持久化确认失败") != std::string::npos);
  CHECK(errors.find("后续降级记录") != std::string::npos);
}

TEST_CASE("替换前失败清理临时文件") {
  TempDirectory temp;
  std::ostringstream out;
  std::ostringstream err;
  const auto file = temp.path() / "node.log";
  auto logger = logging::Logger::Create(FileOptions(file, 128), out, err);
  REQUIRE(logger.has_value());
  (*logger)->Info("填充到需要滚动");
  const ScopedFileFault fault(logging::LoggerTestPeer::FailRename);

  (*logger)->Info(std::string(100, 'R'));

  CHECK(std::distance(std::filesystem::directory_iterator(temp.path()),
                      std::filesystem::directory_iterator{}) == 1);
  CHECK(err.str().find(std::string(100, 'R')) != std::string::npos);
}

TEST_CASE("普通追加部分写失败后回滚且转写完整记录") {
  TempDirectory temp;
  std::ostringstream out;
  std::ostringstream err;
  const auto file = temp.path() / "node.log";
  auto logger = logging::Logger::Create(FileOptions(file), out, err);
  REQUIRE(logger.has_value());
  (*logger)->Info("基线完整行");
  const auto before = ReadText(file);
  const auto size_before = std::filesystem::file_size(file);
  const ScopedFileFault fault(logging::LoggerTestPeer::FailAfterPartialWrite);

  (*logger)->Info("PARTIAL-ID-业务记录");

  const auto text = ReadText(file);
  CHECK(text == before);
  CHECK(std::filesystem::file_size(file) == size_before);
  CHECK(text.find("PARTIAL-ID") == std::string::npos);
  CHECK(text.back() == '\n');
  CHECK(err.str().find("PARTIAL-ID-业务记录") != std::string::npos);
}

TEST_CASE("普通追加完整写后关闭失败不重放业务记录") {
  TempDirectory temp;
  std::ostringstream out;
  std::ostringstream err;
  const auto file = temp.path() / "node.log";
  auto logger = logging::Logger::Create(FileOptions(file), out, err);
  REQUIRE(logger.has_value());
  const ScopedFileFault fault(logging::LoggerTestPeer::FailCloseAfterWriting);

  (*logger)->Info("CLOSE-ID-业务记录");
  (*logger)->Info("关闭失败后降级");

  const auto text = ReadText(file);
  const auto errors = err.str();
  CHECK((text.find("CLOSE-ID") != std::string::npos ? 1 : 0) +
            (errors.find("CLOSE-ID") != std::string::npos ? 1 : 0) ==
        1);
  CHECK(text.back() == '\n');
  CHECK(errors.find("持久化状态不确定") != std::string::npos);
  CHECK(errors.find("关闭失败后降级") != std::string::npos);
}

TEST_CASE("普通追加回滚失败不重放并封闭残留行") {
  TempDirectory temp;
  std::ostringstream out;
  std::ostringstream err;
  const auto file = temp.path() / "node.log";
  auto logger = logging::Logger::Create(FileOptions(file), out, err);
  REQUIRE(logger.has_value());
  const ScopedFileFault fault(logging::LoggerTestPeer::FailPartialWriteAndRollback);

  (*logger)->Info("ROLLBACK-ID-业务记录");
  (*logger)->Info("回滚失败后降级");

  const auto text = ReadText(file);
  const auto errors = err.str();
  CHECK((text.find("ROLLBACK-ID") != std::string::npos ? 1 : 0) +
            (errors.find("ROLLBACK-ID") != std::string::npos ? 1 : 0) ==
        1);
  CHECK(text.back() == '\n');
  CHECK(errors.find("ROLLBACK-ID") == std::string::npos);
  CHECK(errors.find("持久化状态不确定") != std::string::npos);
  CHECK(errors.find("回滚失败后降级") != std::string::npos);
  CHECK(std::distance(std::filesystem::directory_iterator(temp.path()),
                      std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("普通追加首次写失败不依赖回滚即可安全降级") {
  TempDirectory temp;
  std::ostringstream out;
  std::ostringstream err;
  const auto file = temp.path() / "node.log";
  auto logger = logging::Logger::Create(FileOptions(file), out, err);
  REQUIRE(logger.has_value());
  (*logger)->Info("首写失败前基线");
  const auto before = ReadText(file);
  const auto size_before = std::filesystem::file_size(file);
  const ScopedFileFault fault(logging::LoggerTestPeer::FailFirstWriteAndTruncate);

  (*logger)->Info("FIRST-WRITE-ID-完整业务记录");

  CHECK(ReadText(file) == before);
  CHECK(std::filesystem::file_size(file) == size_before);
  CHECK(err.str().find("FIRST-WRITE-ID-完整业务记录") != std::string::npos);
}

TEST_CASE("运行时写入失败只报告一次并持续降级") {
  TempDirectory temp;
  std::ostringstream out;
  std::ostringstream err;
  const auto file = temp.path() / "node.log";
  auto logger = logging::Logger::Create(FileOptions(file), out, err);
  REQUIRE(logger.has_value());
  std::filesystem::remove(file);
  std::filesystem::create_directory(file);

  (*logger)->Info("降级后的第一条");
  (*logger)->Info("降级后的第二条");

  const auto errors = err.str();
  CHECK(std::count(errors.begin(), errors.end(), '\n') == 3);
  CHECK(errors.find("已降级到stderr") != std::string::npos);
  CHECK(errors.find("降级后的第一条") != std::string::npos);
  CHECK(errors.find("降级后的第二条") != std::string::npos);
}

}  // namespace
