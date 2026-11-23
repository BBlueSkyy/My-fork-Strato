// SPDX-License-Identifier: MPL-2.0
#include <gpu/usage_tracker.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
#include <span>

int main() {
    using skyline::gpu::UsageTracker;
    std::array<unsigned char, 1024> bytes{};
    const std::span memory{bytes};
    UsageTracker usage;
    usage.RecordGpuWrite(memory.subspan(32, 16));
    assert(usage.RequiresGpuFlush(memory.subspan(32, 16)));
    assert(usage.HasSequencedWrite(memory.subspan(47, 1)));
    assert(!usage.RequiresGpuFlush(memory.subspan(48, 1)));
    assert(!usage.RequiresGpuFlush(memory.subspan(31, 1)));
    assert(!usage.RequiresGpuFlush(memory.subspan(40, 0)));
    usage.RecordGpuWrite(memory.subspan(0, 0));
    usage.OnSubmit();
    assert(!usage.HasSequencedWrite(memory));
    assert(usage.RequiresGpuFlush(memory)); // Submission is not GPU completion.
    usage.RecordSequencedWrite(memory.subspan(64, 8));
    assert(!usage.RequiresGpuFlush(memory.subspan(64, 8)));
    usage.OnWaitComplete();
    assert(!usage.RequiresGpuFlush(memory));
    assert(usage.HasSequencedWrite(memory.subspan(64, 8))); // Independent lifetimes.
    usage.OnSubmit();

    // The real range set is checked against a byte oracle under fragmentation,
    // duplicate inserts, nesting, adjacency, bridging and out-of-order writes.
    std::mt19937 random{0x244};
    for (unsigned round{}; round < 200; ++round) {
        usage.OnSubmit();
        usage.OnWaitComplete();
        std::array<bool, 1024> written{};
        for (unsigned insert{}; insert < 30; ++insert) {
            auto start{random() % bytes.size()};
            auto size{random() % (bytes.size() - start + 1)};
            usage.RecordGpuWrite(memory.subspan(start, size));
            for (size_t i{start}; i < start + size; ++i) written[i] = true;
            for (unsigned query{}; query < 30; ++query) {
                auto offset{random() % bytes.size()};
                auto length{random() % (bytes.size() - offset + 1)};
                bool overlaps{};
                for (size_t i{offset}; i < offset + length; ++i) overlaps |= written[i];
                assert(usage.RequiresGpuFlush(memory.subspan(offset, length)) == overlaps);
                assert(usage.HasSequencedWrite(memory.subspan(offset, length)) == overlaps);
            }
        }
    }
    std::puts("UsageTracker: boundaries, lifetimes and 180000 randomized overlap queries passed");
}
