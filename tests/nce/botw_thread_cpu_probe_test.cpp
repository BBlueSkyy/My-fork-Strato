// SPDX-License-Identifier: MPL-2.0
#include "../../app/src/main/cpp/skyline/common/botw_thread_cpu_probe.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <ctime>
#include <latch>
#include <pthread.h>
#include <string>
#include <thread>

static std::uint64_t ThreadCpuNs() {
    timespec time{};
    assert(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0);
    return static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}

static void TestInactiveMainThreadRemainsVisible() {
    char originalName[16]{};
    assert(::pthread_getname_np(::pthread_self(), originalName, sizeof(originalName)) == 0);
    assert(::pthread_setname_np(::pthread_self(), "HOS-1") == 0);

    std::latch ready{9}, start{1}, finishedWork{9}, release{1};
    const long ticksPerSecond{::sysconf(_SC_CLK_TCK)};
    assert(ticksPerSecond > 0);
    // Consume five CPU ticks per worker, regardless of host load. The main
    // thread waits on latches, so every worker ranks above it in the sample.
    const auto workNs{5'000'000'000ULL / static_cast<std::uint64_t>(ticksPerSecond)};
    std::array<std::thread, 9> workers;
    for (std::size_t i{}; i < workers.size(); ++i) {
        workers[i] = std::thread([&, i] {
            const auto name{"HOS-" + std::to_string(100 + i)};
            assert(::pthread_setname_np(::pthread_self(), name.c_str()) == 0);
            ready.count_down();
            start.wait();
            const auto before{ThreadCpuNs()};
            while (ThreadCpuNs() - before < workNs)
                std::atomic_signal_fence(std::memory_order_seq_cst);
            finishedWork.count_down();
            release.wait(); // Keep /proc entries alive until the second sample.
        });
    }

    ready.wait();
    skyline::diag::BotwThreadCpuSampler sampler;
    sampler.Snapshot(); // Establish per-thread CPU baselines before work starts.
    start.count_down();
    finishedWork.wait();
    const auto message{sampler.Snapshot()};
    release.count_down();
    for (auto &worker : workers)
        worker.join();
    assert(::pthread_setname_np(::pthread_self(), originalName) == 0);

    const auto mainPosition{message.find("HOS-1(")};
    if (mainPosition == std::string::npos)
        std::fprintf(stderr, "Main guest thread missing: %s\n", message.c_str());
    assert(mainPosition != std::string::npos);
    // Eight CPU-active workers occupy the normal report limit; HOS-1 must
    // appear through the main-thread fallback, rather than through topCount.
    assert(std::count(message.begin(), message.begin() + mainPosition, '(') == 8);
}

int main() {
    std::string stat{"1234 (HOS-7) S"};
    for (int field = 4; field <= 13; ++field)
        stat += " 0";
    stat += " 125 75 0 0";

    auto task{skyline::diag::ParseBotwTaskStat(1234, stat)};
    assert(task.has_value());
    assert(task->tid == 1234);
    assert(task->name == "HOS-7");
    assert(task->state == 'S');
    assert(task->ticks == 200);

    // Linux task names may contain spaces or a literal right parenthesis.
    stat.replace(stat.find("HOS-7"), 5, "HOS-) worker");
    task = skyline::diag::ParseBotwTaskStat(99, stat);
    assert(task && task->name == "HOS-) worker" && task->ticks == 200);

    assert(!skyline::diag::ParseBotwTaskStat(1, "malformed"));
    assert(!skyline::diag::ParseBotwTaskStat(1, "1 (HOS-1) S 2 3"));

    // No special permissions or signal delivery required to inspect our
    // own process; missing /proc will produce a readable diagnostic.
    skyline::diag::BotwThreadCpuSampler sampler;
    const auto message{sampler.Snapshot()};
    assert(!message.empty());
    TestInactiveMainThreadRemainsVisible();
    std::puts("PASS: BOTW guest CPU sampler parses Linux task stats");
}
