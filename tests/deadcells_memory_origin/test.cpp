// SPDX-License-Identifier: MPL-2.0
// Bounded provenance history unit tests, independent of Vulkan/Android.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <gpu/texture/deadcells_memory_origin_history.h>

using namespace skyline::gpu::deadcells_probe::origin;

static_assert(Overlaps(100, 100, 199, 1));
static_assert(!Overlaps(100, 100, 200, 1));
static_assert(!Overlaps(100, 0, 100, 100));
static_assert(Overlaps(UINT64_MAX - 8, 64, UINT64_MAX - 4, 2));
static_assert(!Overlaps(0, 10, 1, 10));

int main() {
    History<3> ring;
    ring.Record({1, 100, "heap_map", 0, 1000, 0, 4096, 0});
    ring.Record({2, 200, "gmmu_map", 5000, 1000, 0x10000, 2048, 24});
    ring.Record({3, 300, "svc_map_copy_dst", 0, 3000, 0, 256, 0});
    int hostHits{}, guestHits{}, bothHits{};
    ring.ForEachOverlap(5001, 1001, 128, [&](const Event &entry, bool host, bool guest) {
        hostHits += host;
        guestHits += guest;
        if (host && guest) {
            ++bothHits;
            assert(entry.serial == 2);
        }
    });
    assert(hostHits == 1 && guestHits == 2 && bothHits == 1);

    ring.Record({4, 400, "gmmu_unmap", 5000, 1000, 0x10000, 2048, 24});
    assert(ring.Total() == 4 && ring.Overwritten() == 1);
    int hits{};
    ring.ForEachOverlap(0, 1001, 128, [&](const Event &, bool, bool) { ++hits; });
    assert(hits == 2); // Oldest heap map is no longer in the finite ring.
    ring.Reset();
    assert(ring.Total() == 0);
    ring.Record({5, 500, "invalid", 0, 0, 0, 64, 0});
    assert(ring.Total() == 0);

    std::puts("Dead Cells origin history: PASS (ranges, aliases, wrap, reset)");
}
