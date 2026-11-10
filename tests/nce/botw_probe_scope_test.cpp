// SPDX-License-Identifier: MPL-2.0
#include "../../app/src/main/cpp/skyline/common/botw_guest_pc_probe.h"
#include "../../app/src/main/cpp/skyline/common/botw_vk_slot_probe.h"
#include <cassert>

int main() {
    using namespace skyline::diag;
    // No title has been loaded. A slow unrelated title must not arm a capture.
    assert(!ArmVkSlotSample());
    RecordBotwGuestPc(1, 0x1234, 0, 0);
    assert(guestPcSlots[1].samples.load() == 0);

    for (const auto title : {std::uint64_t{0}, std::uint64_t{0x0100000000010000ULL}}) {
        ConfigureBotwProbes(title);
        RecordBotwSvc(1, 7);
        IncrementBotwCounter(gpfifoEnqueued, 8);
        IncrementBotwCounter(drawDirectCalls);
        RecordBotwGuestPc(1, 0x1234, 0, 0);
        assert(!ArmVkSlotSample());
        assert(guestSvcEntries.load() == 0);
        assert(guestSvcByThread[1].load() == 0);
        assert(gpfifoEnqueued.load() == 0);
        assert(drawDirectCalls.load() == 0);
        assert(guestPcSlots[1].samples.load() == 0);
    }
    ConfigureBotwProbes(BotwProgramId);
    RecordBotwSvc(1, 7);
    RecordBotwSvc(MaxTrackedGuestThreads, 8); // bounded thread table
    IncrementBotwCounter(gpfifoEnqueued, 8);
    IncrementBotwCounter(drawDirectCalls);
    RecordBotwGuestPc(1, 0x1234, 0, 0);
    assert(ArmVkSlotSample());
    assert(guestSvcEntries.load() == 2);
    assert(guestSvcByThread[1].load() == 1);
    assert(lastSvcId[1].load() == 7);
    assert(gpfifoEnqueued.load() == 8);
    assert(drawDirectCalls.load() == 1);
    assert(guestPcSlots[1].samples.load() == 1);
}
