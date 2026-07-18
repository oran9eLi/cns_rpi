#include "uart/mavlink_port_discovery.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace uart {
namespace {

struct NumberedPath {
  std::string path;
  std::uint64_t number;
};

std::optional<NumberedPath> ParseNumberedPath(const std::string& path,
                                              std::string_view prefix) {
  if (!path.starts_with(prefix)) {
    return std::nullopt;
  }

  const std::string_view suffix(path.data() + prefix.size(),
                                path.size() - prefix.size());
  if (suffix.empty()) {
    return std::nullopt;
  }

  std::uint64_t number = 0;
  const auto [end, error] =
      std::from_chars(suffix.data(), suffix.data() + suffix.size(), number);
  if (error != std::errc{} || end != suffix.data() + suffix.size()) {
    return std::nullopt;
  }
  return NumberedPath{path, number};
}

std::vector<std::string> AlternateEnds(std::vector<NumberedPath> paths) {
  std::ranges::sort(paths, {}, &NumberedPath::number);
  std::vector<std::string> out;
  out.reserve(paths.size());
  std::size_t low = 0;
  std::size_t high = paths.size();
  while (low < high) {
    out.push_back(std::move(paths[low++].path));
    if (low < high) {
      out.push_back(std::move(paths[--high].path));
    }
  }
  return out;
}

}  // 匿名命名空间

std::vector<std::string> OrderSerialCandidates(
    std::span<const std::string> candidates) {
  constexpr std::string_view kUsbPrefix = "/dev/ttyUSB";
  constexpr std::string_view kAcmPrefix = "/dev/ttyACM";
  std::vector<NumberedPath> usb;
  std::vector<NumberedPath> acm;
  std::unordered_set<std::string> seen;

  for (const auto& candidate : candidates) {
    if (!seen.insert(candidate).second) {
      continue;
    }
    if (auto path = ParseNumberedPath(candidate, kUsbPrefix)) {
      usb.push_back(std::move(*path));
    } else if (auto path = ParseNumberedPath(candidate, kAcmPrefix)) {
      acm.push_back(std::move(*path));
    }
  }

  auto ordered = AlternateEnds(std::move(usb));
  auto ordered_acm = AlternateEnds(std::move(acm));
  ordered.reserve(ordered.size() + ordered_acm.size());
  std::ranges::move(ordered_acm, std::back_inserter(ordered));
  return ordered;
}

std::vector<std::string> EnumerateSerialCandidates() {
  std::vector<std::string> candidates;
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      "/dev", std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    std::error_code type_error;
    if (iterator->is_character_file(type_error) && !type_error) {
      candidates.push_back(iterator->path().string());
    }
    iterator.increment(error);
  }
  return OrderSerialCandidates(candidates);
}

}  // 命名空间 uart
