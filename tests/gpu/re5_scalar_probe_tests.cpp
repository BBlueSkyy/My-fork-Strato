// SPDX-License-Identifier: MPL-2.0
#include <gpu/diagnostic/re5_scalar_probe.h>
#include <gpu/diagnostic/re5_scene_capture.h>
#include <cassert>
#include <limits>
#include <array>
#include <vector>

int main() {
    using namespace skyline::gpu::diagnostic;
    assert(ScalarRangeValid(452, 448));
    assert(!ScalarRangeValid(451, 448));
    assert(!ScalarRangeValid(0, 0));
    assert(ScalarRangeValid(4, 0));
    assert(!ScalarRangeValid(std::numeric_limits<std::uint64_t>::max(),
                             std::numeric_limits<std::uint64_t>::max() - 2));
    assert(IdentityCopyCovers(7, 2, 7, 0, 7, 0, 3));
    assert(!IdentityCopyCovers(7, 3, 7, 0, 7, 0, 3));
    assert(!IdentityCopyCovers(7, 2, 8, 0, 7, 0, 3));
    assert(!IdentityCopyCovers(7, 2, 7, 0, 7, 1, 3));
    assert(!IdentityCopyCovers(7, 0, 7, 0, 7, 0, 0));
    struct Slot { const int *info{}; unsigned binding{}, element{}; };
    struct Copy { unsigned srcBinding, srcArrayElement, dstBinding, dstArrayElement, descriptorCount; };
    const int original{1}, replacement{2};
    std::array<Slot, 2> slots{{{&original, 7, 2}, {&original, 9, 0}}};
    std::array<Slot, 2> writes{{{}, {&replacement, 9, 0}}};
    std::vector<Copy> copies{{7, 0, 7, 0, 3}, {9, 0, 9, 0, 1}};
    UpdateScalarProvenance(slots, writes, copies);
    assert(slots[0].info == &original && slots[0].binding == 7 && slots[0].element == 2);
    assert(slots[1].info == &replacement);
    copies = {{7, 0, 8, 0, 3}};
    writes = {};
    UpdateScalarProvenance(slots, writes, copies);
    assert(!slots[0].info && !slots[1].info);
    writes[0] = {&replacement, 3, 0};
    copies.clear();
    UpdateScalarProvenance(slots, writes, copies);
    assert(slots[0].info == &replacement && !slots[1].info);
    UpdateScalarProvenance(slots, std::array<Slot, 2>{}, copies);
    assert(!slots[0].info);
    SceneFrameCapture capture;
    assert(!capture.ReserveScalarProbe(0));
    capture.Start("first");
    for (unsigned interval = 0; interval < 4; ++interval) {
        for (unsigned i = 0; i < 32; ++i) {
            auto ticket = capture.ReserveScalarProbe(interval);
            assert(ticket && ticket->index == interval * 32 + i);
            assert(ticket->directory == "first");
        }
        assert(!capture.ReserveScalarProbe(interval));
    }
    assert(!capture.ReserveScalarProbe(4));
    capture.active.store(false);
    assert(!capture.ReserveScalarProbe(0));
    capture.Start("second");
    auto ticket = capture.ReserveScalarProbe(0);
    assert(ticket && ticket->index == 0 && ticket->directory == "second");
    for (unsigned target = 0; target < 2; ++target)
        for (unsigned i = 0; i < 100; ++i)
            assert(capture.SampleScalarDraw(0, target) == (i < 8 || i % 32 == 0));
    assert(!capture.SampleScalarDraw(4, 0));
    assert(!capture.SampleScalarDraw(0, 2));
}
