// SPDX-License-Identifier: MPL-2.0
#include <gpu/texture/staging_retirement.h>
#include <cassert>
#include <thread>
#include <vector>
#include <iostream>
using skyline::gpu::texture::StagingRetirementStats;
int main() {
    StagingRetirementStats stats;
    stats.SetIdentity("Test game", "0100123456789000");
    assert(stats.GameName() == "Test game" && stats.TitleId() == "0100123456789000");
    assert(!stats.RecordBatch(0, 0).report);
    auto first{stats.RecordBatch(2, 4096)};
    assert(first.report && first.operations == 1 && first.buffers == 2 && first.bytes == 4096);
    auto second{stats.RecordBatch(1, 1024)};
    assert(!second.report && second.operations == 2 && second.buffers == 3 && second.bytes == 5120);
    // Each completed batch contributes exactly once; no reporting on empty drops.
    std::vector<std::thread> workers;
    for (int i{}; i < 4; ++i) workers.emplace_back([&] { for (int j{}; j < 1000; ++j) stats.RecordBatch(1, 64); });
    for (auto &worker : workers) worker.join();
    auto result{stats.Snapshot()};
    assert(result.operations == 4002 && result.buffers == 4003 && result.bytes == 261120);
    assert(!stats.RecordBatch(0, 0).report && stats.Snapshot().operations == 4002);
    std::cout << "PASS staging retirement per-game counters, batched bytes, sparse reports and concurrent cold-path updates\n";
}
