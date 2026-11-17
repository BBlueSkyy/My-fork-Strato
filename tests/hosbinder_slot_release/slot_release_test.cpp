// SPDX-License-Identifier: MIT OR MPL-2.0
// Native, dependency-free regression for the Dead Cells queue generation race.
#include <cstdint>
#include <cstdio>
#include <services/hosbinder/buffer_slot_release_guard.h>

using skyline::service::hosbinder::ShouldReleaseQueuedFrame;

static_assert(ShouldReleaseQueuedFrame(7, 7, true));
static_assert(!ShouldReleaseQueuedFrame(7, 6, true));
static_assert(!ShouldReleaseQueuedFrame(7, 7, false));
static_assert(!ShouldReleaseQueuedFrame(0, 7, false));

struct Slot {
    std::uint64_t frame{};
    bool queued{};

    void Queue(std::uint64_t n) { frame = n; queued = true; }
    void Complete(std::uint64_t n) {
        if (ShouldReleaseQueuedFrame(frame, n, queued)) queued = false;
    }
};

int main() {
    Slot slot;
    // Captured sequence: event 283 queues frame 7, event 284 is
    // the delayed release of frame 6, then frame 7 is presented.
    slot.Queue(7);
    slot.Complete(6);
    if (!slot.queued || slot.frame != 7) return 1;
    slot.Complete(7);
    if (slot.queued) return 2;

    // A stale presentation release must not free a newer queued frame.
    slot.Queue(132);
    slot.Queue(133);
    slot.Complete(132);
    if (!slot.queued || slot.frame != 133) return 3;
    slot.Complete(133);
    if (slot.queued) return 4;

    // If the slot is not queued, a callback cannot modify it.
    slot.frame = 0;
    slot.queued = false;
    slot.Complete(133);
    if (slot.queued || slot.frame) return 5;

    std::puts("hosbinder stale presentation callback guard: PASS");
    return 0;
}
