// SPDX-License-Identifier: MPL-2.0
// Temporary BOTW softlock diagnostic: samples *our own* HOS/GPFIFO host
// threads. CPU time is not proof of guest game-state progress.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include "botw_freeze_probe.h"

namespace skyline::diag {
    struct BotwTaskCpu {
        int tid{};
        std::string name;
        char state{'?'};
        std::uint64_t ticks{};
    };

    // /proc/<pid>/task/<tid>/stat fields: pid (comm) state, fields 4-13,
    // utime (14), stime (15). The command may contain spaces or parentheses.
    inline std::optional<BotwTaskCpu> ParseBotwTaskStat(int tid, std::string_view line) {
        const auto open{line.find('(')};
        const auto close{line.rfind(')')};
        if (open == std::string_view::npos || close == std::string_view::npos || close <= open)
            return std::nullopt;

        std::istringstream values{std::string(line.substr(close + 1))};
        char state{};
        if (!(values >> state))
            return std::nullopt;
        std::string ignored;
        for (int field = 4; field <= 13; ++field)
            if (!(values >> ignored))
                return std::nullopt;

        std::uint64_t userTicks{}, systemTicks{};
        if (!(values >> userTicks >> systemTicks))
            return std::nullopt;
        return BotwTaskCpu{tid, std::string(line.substr(open + 1, close - open - 1)),
                           state, userTicks + systemTicks};
    }

    class BotwThreadCpuSampler {
        std::unordered_map<int, std::uint64_t> previousTicks;
        std::array<std::uint64_t, MaxTrackedGuestThreads> previousGuestSvc{};

      public:
        std::string Snapshot() {
            DIR *dir{::opendir("/proc/self/task")};
            if (!dir)
                return "proc_task_unavailable";

            struct Delta {
                BotwTaskCpu sample;
                std::uint64_t cpuMs{};
                std::uint64_t svcDelta{};
                std::uint32_t svcId{};
            };

            const long rawClockTicks{::sysconf(_SC_CLK_TCK)};
            const std::uint64_t ticksPerSecond{rawClockTicks > 0 ? static_cast<std::uint64_t>(rawClockTicks) : 100};
            std::vector<Delta> active;
            std::unordered_map<int, std::uint64_t> currentTicks;
            std::size_t hosTotal{}, hosCpuActive{}, hosSleeping{}, hosRunnable{};
            std::uint64_t hosCpuTotalMs{};

            while (auto *item = ::readdir(dir)) {
                if (!std::isdigit(static_cast<unsigned char>(item->d_name[0])))
                    continue;
                const int tid{std::atoi(item->d_name)};
                if (tid <= 0)
                    continue;

                std::ifstream input{"/proc/self/task/" + std::to_string(tid) + "/stat"};
                std::string line;
                if (!std::getline(input, line))
                    continue;
                auto parsed{ParseBotwTaskStat(tid, line)};
                if (!parsed)
                    continue;

                const bool hos{parsed->name.rfind("HOS-", 0) == 0};
                if (!hos && parsed->name != "GPFIFO" && parsed->name != "Sky-Present")
                    continue;

                const auto prev{previousTicks.find(tid)};
                const std::uint64_t deltaTicks{prev != previousTicks.end() && parsed->ticks >= prev->second
                                                ? parsed->ticks - prev->second : 0};
                currentTicks[tid] = parsed->ticks;
                const std::uint64_t cpuMs{deltaTicks * 1000 / ticksPerSecond};

                std::uint64_t svcDelta{};
                std::uint32_t lastSvc{};
                if (hos) {
                    ++hosTotal;
                    hosCpuTotalMs += cpuMs;
                    if (cpuMs)
                        ++hosCpuActive;
                    if (parsed->state == 'S' || parsed->state == 'D')
                        ++hosSleeping;
                    if (parsed->state == 'R')
                        ++hosRunnable;

                    char *end{};
                    const unsigned long id{std::strtoul(parsed->name.c_str() + 4, &end, 10)};
                    if (end && *end == '\0' && id < MaxTrackedGuestThreads) {
                        const auto now{guestSvcByThread[id].load(std::memory_order_relaxed)};
                        svcDelta = now >= previousGuestSvc[id] ? now - previousGuestSvc[id] : 0;
                        previousGuestSvc[id] = now;
                        lastSvc = lastSvcId[id].load(std::memory_order_relaxed);
                    }
                }
                active.push_back({std::move(*parsed), cpuMs, svcDelta, lastSvc});
            }

            ::closedir(dir);
            previousTicks.swap(currentTicks);

            std::sort(active.begin(), active.end(), [](const Delta &a, const Delta &b) {
                if (a.cpuMs != b.cpuMs)
                    return a.cpuMs > b.cpuMs;
                return a.sample.name < b.sample.name;
            });

            std::ostringstream details;
            details << "HOS total=" << hosTotal << " cpuActive=" << hosCpuActive
                    << " sleeping=" << hosSleeping << " runnable=" << hosRunnable
                    << " cpuTotalMs=" << hosCpuTotalMs << " threads=";
            auto print = [&](const Delta &task) {
                details << task.sample.name << "(" << task.sample.state << ")"
                        << ":cpu+" << task.cpuMs << "ms"
                        << ":svc+" << task.svcDelta
                        << ":lastSVC=0x" << std::hex << task.svcId << std::dec << ",";
            };
            const std::size_t topCount{std::min<std::size_t>(active.size(), 8)};
            for (std::size_t i{}; i < topCount; ++i)
                print(active[i]);

            // Always include the main guest thread and the thread of the
            // previous BOTW crash even when they are inactive.
            for (const auto name : {"HOS-0", "HOS-7"}) {
                auto it{std::find_if(active.begin(), active.end(),
                                     [name](const Delta &task) { return task.sample.name == name; })};
                if (it != active.end() && static_cast<std::size_t>(it - active.begin()) >= topCount)
                    print(*it);
            }
            return details.str();
        }
    };
}
