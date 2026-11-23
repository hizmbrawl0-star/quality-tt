#pragma once

#include <cstdint>
#include <functional>
#include <string>

using PatchLog = std::function<void(const std::string&)>;
using PatchProgress = std::function<void(uint64_t written, uint64_t total)>;

void patchVideo(const std::string& source, const std::string& destination,
                const PatchLog& log, const PatchProgress& progress);
