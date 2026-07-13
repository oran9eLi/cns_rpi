/**
 * @file config_store.cpp
 * @brief config_store.hpp 的 POSIX 实现。
 */

#include "config_command/config_store.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace config_command {
namespace {

CommandError WriteError(std::string message = "配置持久化失败") {
  return {.code = "config_write_failed", .message = std::move(message)};
}

CommandError UncertainError() {
  return {.code = "config_write_uncertain", .message = "配置提交状态不确定"};
}

bool WriteAll(int fd, std::string_view content) {
  std::size_t offset = 0;
  while (offset < content.size()) {
    const auto written = ::write(fd, content.data() + offset, content.size() - offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

std::expected<std::filesystem::path, CommandError> WriteCandidate(
    const std::filesystem::path& config_path, const nlohmann::json& candidate) {
  const auto parent = config_path.parent_path().empty() ? std::filesystem::path{"."}
                                                        : config_path.parent_path();
  std::string pattern = (parent / ("." + config_path.filename().string() + ".tmp.XXXXXX")).string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const int fd = ::mkstemp(writable.data());
  if (fd < 0) return std::unexpected(WriteError());

  const std::filesystem::path candidate_path{writable.data()};
  const std::string content = candidate.dump(2) + "\n";
  const bool success = WriteAll(fd, content) && ::fsync(fd) == 0 && ::close(fd) == 0;
  if (!success) {
    ::close(fd);
    ::unlink(candidate_path.c_str());
    return std::unexpected(WriteError());
  }
  return candidate_path;
}

bool FsyncDirectory(const std::filesystem::path& path) {
  const auto directory = path.parent_path().empty() ? std::filesystem::path{"."}
                                                    : path.parent_path();
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return false;
  const bool success = ::fsync(fd) == 0;
  ::close(fd);
  return success;
}

bool FsyncFile(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  const bool success = ::fsync(fd) == 0;
  ::close(fd);
  return success;
}

std::expected<void, CommandError> PersistDirect(const std::filesystem::path& config_path,
                                                const nlohmann::json& candidate) {
  auto temporary = WriteCandidate(config_path, candidate);
  if (!temporary) return std::unexpected(temporary.error());
  const std::filesystem::path backup = temporary->string() + ".old";
  if (::link(config_path.c_str(), backup.c_str()) != 0 || !FsyncDirectory(config_path)) {
    ::unlink(temporary->c_str());
    ::unlink(backup.c_str());
    return std::unexpected(WriteError());
  }
  if (::rename(temporary->c_str(), config_path.c_str()) != 0) {
    ::unlink(temporary->c_str());
    ::unlink(backup.c_str());
    return std::unexpected(WriteError());
  }
  if (!FsyncDirectory(config_path)) {
    const bool restore_renamed = ::rename(backup.c_str(), config_path.c_str()) == 0;
    if (!restore_renamed || !FsyncDirectory(config_path)) {
      return std::unexpected(UncertainError());
    }
    return std::unexpected(WriteError());
  }
  ::unlink(backup.c_str());
  // 新配置已经完成目录fsync，此后的备份清理失败不改变提交结果。
  FsyncDirectory(config_path);
  return {};
}

std::expected<void, CommandError> PersistWithHelper(const WriterOptions& options,
                                                    const std::filesystem::path& config_path,
                                                    const nlohmann::json& candidate) {
  auto temporary = WriteCandidate(config_path, candidate);
  if (!temporary) return std::unexpected(temporary.error());

  const std::string helper = options.helper_path.string();
  const std::string source = temporary->string();
  const std::string target = config_path.string();
  const pid_t child = ::fork();
  if (child == 0) {
    ::execl(helper.c_str(), helper.c_str(), source.c_str(), target.c_str(),
            static_cast<char*>(nullptr));
    _exit(127);
  }
  if (child < 0) {
    ::unlink(temporary->c_str());
    return std::unexpected(WriteError());
  }
  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      ::unlink(temporary->c_str());
      return std::unexpected(WriteError());
    }
  }
  ::unlink(temporary->c_str());
  bool target_matches = false;
  try {
    std::ifstream input(config_path);
    nlohmann::json verified;
    input >> verified;
    target_matches = input && verified == candidate;
  } catch (const nlohmann::json::exception&) {
    target_matches = false;
  }
  if (target_matches) {
    if (!FsyncFile(config_path) || !FsyncDirectory(config_path)) {
      return std::unexpected(UncertainError());
    }
    // 即使helper错误地返回非零，目标内容和持久化边界均已确认，不能再报告普通失败。
    return {};
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::unexpected(WriteError());
  return std::unexpected(WriteError("helper返回成功但目标配置未更新"));
}

}  // namespace

std::expected<WriterOptions, std::string> ParseWriterOptions(
    const std::vector<std::string_view>& arguments) {
  WriterOptions options;
  bool writer_seen = false;
  bool helper_seen = false;
  for (const auto argument : arguments) {
    constexpr std::string_view writer_prefix = "--config-writer=";
    constexpr std::string_view helper_prefix = "--config-helper=";
    if (argument.starts_with(writer_prefix)) {
      if (writer_seen) return std::unexpected("配置写入模式重复");
      writer_seen = true;
      const auto value = argument.substr(writer_prefix.size());
      if (value == "disabled") options.mode = ConfigWriterMode::kDisabled;
      else if (value == "direct") options.mode = ConfigWriterMode::kDirect;
      else if (value == "helper") options.mode = ConfigWriterMode::kHelper;
      else return std::unexpected("配置写入模式非法");
    } else if (argument.starts_with(helper_prefix)) {
      if (helper_seen) return std::unexpected("配置辅助程序路径重复");
      helper_seen = true;
      options.helper_path = argument.substr(helper_prefix.size());
      if (options.helper_path.empty()) return std::unexpected("配置辅助程序路径为空");
    } else {
      return std::unexpected("未知启动参数");
    }
  }
  if (options.mode == ConfigWriterMode::kHelper && options.helper_path.empty()) {
    return std::unexpected("helper模式缺少辅助程序路径");
  }
  if (options.mode != ConfigWriterMode::kHelper && helper_seen) {
    return std::unexpected("非helper模式不能指定辅助程序");
  }
  return options;
}

std::expected<void, CommandError> PersistConfig(
    const WriterOptions& options, const std::filesystem::path& config_path,
    const nlohmann::json& candidate) {
  switch (options.mode) {
    case ConfigWriterMode::kDisabled:
      return std::unexpected(CommandError{.code = "config_write_disabled",
                                          .message = "配置写入功能未启用"});
    case ConfigWriterMode::kDirect:
      return PersistDirect(config_path, candidate);
    case ConfigWriterMode::kHelper:
      return PersistWithHelper(options, config_path, candidate);
  }
  return std::unexpected(WriteError());
}

}  // namespace config_command
