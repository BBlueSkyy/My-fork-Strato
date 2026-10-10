#include <array>
#include <cassert>
#include <skyline/gpu/usage_tracker.h>

using namespace skyline;
using namespace skyline::gpu;

int main() {
    std::array<u8, 256> memory{};
    span<u8> all{memory};

    UsageTracker tracker;

    auto dirty{all.subspan(32, 32)};
    tracker.MarkGpuDirty(dirty);
    assert(tracker.IntersectsGpuDirty(dirty));
    assert(tracker.IntersectsGpuDirty(all.subspan(48, 4)));
    assert(!tracker.IntersectsGpuDirty(all.subspan(0, 32)));
    assert(!tracker.IntersectsGpuDirty(all.subspan(64, 16)));

    tracker.MarkSequencedWrite(all.subspan(136, 8));
    assert(tracker.IntersectsSequencedWrite(all.subspan(136, 1)));
    assert(!tracker.IntersectsSequencedWrite(all.subspan(128, 8)));
    assert(!tracker.IntersectsSequencedWrite(dirty));

    tracker.ResetSequencedWrites();
    assert(!tracker.IntersectsSequencedWrite(all.subspan(136, 8)));
    assert(tracker.IntersectsGpuDirty(dirty));

    tracker.ResetGpuDirty();
    assert(!tracker.IntersectsGpuDirty(dirty));

    tracker.MarkGpuDirty(all.subspan(64, 16));
    tracker.MarkGpuDirty(all.subspan(80, 16));
    assert(tracker.IntersectsGpuDirty(all.subspan(95, 1)));
    assert(!tracker.IntersectsGpuDirty(all.subspan(96, 8)));

    tracker.MarkSequencedWrite(all.subspan(8, 8));
    tracker.MarkSequencedWrite(all.subspan(24, 8));
    assert(!tracker.IntersectsSequencedWrite(all.subspan(16, 8)));

    span<u8> empty{};
    tracker.MarkGpuDirty(empty);
    tracker.MarkSequencedWrite(empty);
    assert(!tracker.IntersectsGpuDirty(empty));
    assert(!tracker.IntersectsSequencedWrite(empty));
}
