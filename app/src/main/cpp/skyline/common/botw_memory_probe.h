// SPDX-License-Identifier: MPL-2.0
// Temporary, read-only memory telemetry for BOTW visual-freeze diagnosis.
#pragma once

#include <charconv>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace skyline::diag {
    // All values are KiB; -1 means the corresponding procfs counter was
    // unavailable. VmSize is address space, NOT committed/resident memory.
    struct BotwProcMemory {
        std::int64_t vmRssKiB{-1};
        std::int64_t vmHwmKiB{-1};
        std::int64_t rssAnonKiB{-1};
        std::int64_t rssFileKiB{-1};
        std::int64_t vmSwapKiB{-1};
        std::int64_t vmSizeKiB{-1};
        std::int64_t memAvailableKiB{-1};
        std::int64_t swapFreeKiB{-1};
    };

    inline std::int64_t ParseBotwProcKiB(std::string_view line, std::string_view key) {
        if (!line.starts_with(key))
            return -1;

        const auto start{line.find_first_of("0123456789", key.size())};
        if (start == std::string_view::npos)
            return -1;

        std::int64_t value{};
        const auto result{std::from_chars(line.data() + start, line.data() + line.size(), value)};
        return result.ec == std::errc{} ? value : -1;
    }

    inline BotwProcMemory SampleBotwProcMemory() {
        BotwProcMemory result{};
        std::string line;
        std::ifstream status{"/proc/self/status"};
        while (std::getline(status, line)) {
            if (line.starts_with("VmRSS:"))
                result.vmRssKiB = ParseBotwProcKiB(line, "VmRSS:");
            else if (line.starts_with("VmHWM:"))
                result.vmHwmKiB = ParseBotwProcKiB(line, "VmHWM:");
            else if (line.starts_with("RssAnon:"))
                result.rssAnonKiB = ParseBotwProcKiB(line, "RssAnon:");
            else if (line.starts_with("RssFile:"))
                result.rssFileKiB = ParseBotwProcKiB(line, "RssFile:");
            else if (line.starts_with("VmSwap:"))
                result.vmSwapKiB = ParseBotwProcKiB(line, "VmSwap:");
            else if (line.starts_with("VmSize:"))
                result.vmSizeKiB = ParseBotwProcKiB(line, "VmSize:");
        }

        std::ifstream meminfo{"/proc/meminfo"};
        while (std::getline(meminfo, line)) {
            if (line.starts_with("MemAvailable:"))
                result.memAvailableKiB = ParseBotwProcKiB(line, "MemAvailable:");
            else if (line.starts_with("SwapFree:"))
                result.swapFreeKiB = ParseBotwProcKiB(line, "SwapFree:");
        }
        return result;
    }
}
