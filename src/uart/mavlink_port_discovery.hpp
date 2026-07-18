#pragma once

#include <span>
#include <string>
#include <vector>

namespace uart {

std::vector<std::string> OrderSerialCandidates(
    std::span<const std::string> candidates);

std::vector<std::string> EnumerateSerialCandidates();

}  // 命名空间 uart
