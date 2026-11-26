// SPDX-License-Identifier: MPL-2.0
// Micro-benchmark of the TrapManager protection paths (TrapRegions/RemoveTrap/DeleteTrap). Not part of CI, see bench.py.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sys/mman.h>
#include "trap_manager.h"

using namespace skyline;

extern "C" int __real_mprotect(void *, size_t, int);
static unsigned long mprotectCalls;
static bool skipSyscall; //!< STRATO_BENCH_NOSYSCALL=1 measures only the user-space bookkeeping around mprotect
extern "C" int __wrap_mprotect(void *address, size_t size, int protection) {
    mprotectCalls++;
    return skipSyscall ? 0 : __real_mprotect(address, size, protection);
}

namespace {
    constexpr size_t Page{constant::PageSize};
    using Clock = std::chrono::steady_clock;

    struct Workload {
        const char *name;
        size_t traps;        //!< live traps
        unsigned overlapPct; //!< percentage of traps that are placed on top of an earlier trap, partially overlapping it
        size_t maxPages;     //!< largest trap in pages
    };

    TrapHandle Make(TrapManager &trap, u8 *begin, size_t size) {
        std::vector<span<u8>> regions{span<u8>{begin, size}};
        return trap.CreateTrap(span<span<u8>>{regions}, [] {}, [] { return true; }, [] { return true; });
    }

    void Run(const Workload &workload, unsigned iterations) {
        const size_t arenaPages{workload.traps * workload.maxPages * 2 + 64};
        auto *base{static_cast<u8 *>(mmap(nullptr, arenaPages * Page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0))};
        if (base == MAP_FAILED) { std::perror("mmap"); std::exit(2); }

        std::mt19937 rng{1234};
        TrapManager trap;
        std::vector<TrapHandle> handles;
        std::vector<std::pair<size_t, size_t>> placed; // page, pages
        size_t cursor{};
        for (size_t i{}; i < workload.traps; i++) {
            // Mostly small buffers/textures with a long tail, like a streaming title
            size_t pages{1 + (rng() % 100 < 85 ? rng() % 8 : rng() % workload.maxPages)};
            size_t page{cursor};
            if (!placed.empty() && rng() % 100 < workload.overlapPct) {
                const auto &[otherPage, otherPages]{placed[rng() % placed.size()]};
                page = otherPage + rng() % otherPages; // starts inside an existing trap, so the two overlap partially
            } else {
                cursor += pages + 1;
            }
            placed.emplace_back(page, pages);
            handles.push_back(Make(trap, base + page * Page + (rng() % 3) * 0x100, pages * Page - 0x80));
        }

        // Steady state: GPU-dirty / CPU-dirty ping-pong, the common TrapRegions caller pattern
        auto start{Clock::now()};
        const auto callsBefore{mprotectCalls};
        for (unsigned i{}; i < iterations; i++) {
            auto &handle{handles[rng() % handles.size()]};
            trap.TrapRegions(handle, (i & 1) != 0);
        }
        const double trapNs{std::chrono::duration<double, std::nano>(Clock::now() - start).count() / iterations};
        const double trapCalls{static_cast<double>(mprotectCalls - callsBefore) / iterations};

        start = Clock::now();
        const auto removeBefore{mprotectCalls};
        for (unsigned i{}; i < iterations; i++)
            trap.RemoveTrap(handles[rng() % handles.size()]);
        const double removeNs{std::chrono::duration<double, std::nano>(Clock::now() - start).count() / iterations};
        const double removeCalls{static_cast<double>(mprotectCalls - removeBefore) / iterations};

        // Churn: create, trap, delete (buffer/texture lifetime)
        start = Clock::now();
        const auto churnBefore{mprotectCalls};
        for (unsigned i{}; i < iterations / 4; i++) {
            const auto &[page, pages]{placed[rng() % placed.size()]};
            auto handle{Make(trap, base + page * Page, pages * Page)};
            trap.TrapRegions(handle, true);
            trap.DeleteTrap(handle);
        }
        const double churnNs{std::chrono::duration<double, std::nano>(Clock::now() - start).count() / (iterations / 4)};
        const double churnCalls{static_cast<double>(mprotectCalls - churnBefore) / (iterations / 4)};

        std::printf("%-26s TrapRegions %8.0f ns (%.2f mprotect)  RemoveTrap %8.0f ns (%.2f)  create+trap+delete %8.0f ns (%.2f)\n",
                    workload.name, trapNs, trapCalls, removeNs, removeCalls, churnNs, churnCalls);

        for (auto &handle : handles)
            trap.DeleteTrap(handle);
        munmap(base, arenaPages * Page);
    }
}

int main(int argc, char **argv) {
    const unsigned iterations{argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 20000};
    skipSyscall = std::getenv("STRATO_BENCH_NOSYSCALL") != nullptr;
    Run({"few traps (200)", 200, 10, 64}, iterations);
    Run({"typical (2000)", 2000, 10, 128}, iterations);
    Run({"many traps (6000)", 6000, 10, 128}, iterations);
    Run({"overlap heavy (2000, 60%)", 2000, 60, 64}, iterations);
    Run({"no overlap (2000)", 2000, 0, 128}, iterations);
    return 0;
}
