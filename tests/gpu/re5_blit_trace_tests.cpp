// SPDX-License-Identifier: MPL-2.0
#include <gpu/diagnostic/re5_blit_trace.h>
#include <gpu/diagnostic/re5_scene_capture.h>
#include <cassert>
#include <thread>

int main() {
    using namespace skyline::gpu::diagnostic;
    BlitJournal journal;
    journal.Store({1, 100, "first", {{0x1000, 0x100}}, 0});
    journal.Store({2, 100, "replacement", {{0x1000, 0x100}}, 0});
    journal.BeginCapture("a");
    journal.TrackTexture(100);
    journal.NoteSync(100, "upload_copy_recorded");
    journal.NoteSync(100, "download_copy_recorded");
    assert(journal.Snapshot("a").find("uploads=1 downloads=1") != std::string::npos);
    auto direct = journal.PinRelated("a", 100, {});
    assert(direct.size() == 1 && direct.front() == 2);
    journal.Store({3, 100, "later", {{0x1000, 0x100}}, 0});
    assert(journal.Snapshot("a").find("replacement") != std::string::npos);
    assert(journal.Snapshot("a").find("later") != std::string::npos);
    auto overlap = journal.PinRelated("a", 200, {{0x1080, 0x10}});
    assert(overlap.size() == 1 && overlap.front() == 3);
    assert(journal.PinRelated("a", 200, {{0x1100, 0x10}}).empty());
    const auto frozen{journal.FreezeSnapshot("a")};
    journal.Store({4, 300, "after_freeze", {{0x3000, 4}}, 0});
    assert(journal.PinRelated("a", 300, {}).empty());
    assert(frozen.find("after_freeze") == std::string::npos);
    assert(!RangesOverlap({UINT64_MAX - 4, 8}, {0, 8}));
    journal.BeginCapture("b");
    assert(journal.PinRelated("a", 100, {}).empty());
    assert(journal.Snapshot("b").find("replacement") == std::string::npos);
    for (unsigned i = 0; i < BlitJournal::MaxTextures + 10; ++i)
        journal.Store({1000 + i, 1000 + i, std::string(BlitJournal::MaxRecordBytes + 100, 'x'), {}, 0});
    assert(journal.Size() == BlitJournal::MaxTextures);
    assert(journal.Snapshot("b").size() <= BlitJournal::MaxSnapshotBytes);
    assert(journal.Snapshot("b").find("evictions=") != std::string::npos);
    assert(!CurrentBlit());
    {
        BlitScope first;
        assert(CurrentBlit() == &first);
        {
            BlitScope nested;
            assert(CurrentBlit() == &nested);
        }
        assert(CurrentBlit() == &first);
        std::thread other([] { assert(!CurrentBlit()); });
        other.join();
    }
    assert(!CurrentBlit());
    struct Slot { unsigned binding{}, element{}; const int *info{}; };
    struct Copy { unsigned srcBinding, srcArrayElement, dstBinding, dstArrayElement, descriptorCount; };
    const int old{1}, fresh{2};
    std::vector<Slot> slots{{7, 2, &old}, {9, 0, &old}};
    std::vector<Slot> writes{{9, 0, &fresh}};
    UpdateImageProvenance(slots, writes, std::vector<Copy>{{7, 0, 7, 0, 3}}, 16);
    assert(slots.size() == 2 && slots[0].info == &old && slots[1].info == &fresh);
    UpdateImageProvenance(slots, std::vector<Slot>{}, std::vector<Copy>{{7, 0, 8, 0, 3}}, 16);
    assert(slots.empty());
    SceneFrameCapture capture;
    capture.Start("sampling");
    for (unsigned group = 0; group < 5; ++group) {
        unsigned sampled{};
        for (unsigned i = 0; i < 200; ++i)
            sampled += capture.ReserveBindingProbe(0, group).has_value();
        assert(sampled == (group < 4 ? 4 : 2));
    }
    assert(!capture.ReserveBindingProbe(4, 0));
    assert(!capture.ReserveBindingProbe(0, 5));
}
