// SPDX-License-Identifier: MPL-2.0
#include "../../app/src/main/cpp/skyline/gpu/texture/guest_bridge_ledger.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace skyline::gpu::texture;
using K = GuestBridgeResourceKind;
using P = GuestBridgeParticipant;
static bool SameCells(const std::vector<GuestBridgeCell> &a, const std::vector<GuestBridgeCell> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i{}; i < a.size(); ++i)
        if (a[i].begin != b[i].begin || a[i].end != b[i].end ||
            a[i].hostVersion != b[i].hostVersion || a[i].guestVersion != b[i].guestVersion ||
            a[i].writerToken != b[i].writerToken || a[i].writerKind != b[i].writerKind)
            return false;
    return true;
}
int main() {
    GuestBridgeLedger ledger;
    P buffer{1,K::Buffer,{{0x1000,0x1200}}};
    P image{2,K::Image,{{0x1080,0x10a0},{0x10c0,0x1100}}};
    assert(ledger.Join(std::vector<P>{buffer}));
    assert(ledger.RecordWrite(1,K::Buffer,0x1000,0x1200,7));
    assert(!ledger.ConfirmGuestPublication(1,K::Buffer,0x1000,0x1200,6));
    assert(ledger.ConfirmGuestPublication(1,K::Buffer,0x1000,0x1200,7));
    assert(ledger.Join(std::vector<P>{buffer,image}));
    for (const auto &cell : ledger.Cells())
        assert(cell.writerToken == 1 && cell.hostVersion == 7 && cell.guestVersion == 7);
    assert(ledger.RecordWrite(2,K::Image,0x1080,0x10a0,8));
    assert(!ledger.RecordWrite(2,K::Image,0x10a0,0x10c0,9)); // padding is buffer-only
    assert(!ledger.ConfirmGuestPublication(1,K::Buffer,0x1080,0x10a0,7));
    assert(ledger.ConfirmGuestPublication(2,K::Image,0x1080,0x10a0,8));
    assert(ledger.RecordWrite(1,K::Buffer,0x1090,0x10d0,9));
    assert(ledger.RecordWrite(1,K::Buffer,0x1000,0x1080,10));
    // Confirming only a subset cannot advance the guest version of its siblings.
    assert(ledger.ConfirmGuestPublication(1,K::Buffer,0x1010,0x1020,10));
    for (const auto &cell : ledger.Cells())
        if (cell.begin == 0x1010 && cell.end == 0x1020)
            assert(cell.guestVersion == 10);
        else if (cell.begin >= 0x1000 && cell.end <= 0x1080)
            assert(cell.hostVersion == 10 && cell.guestVersion == 7);
    assert(!ledger.ConfirmGuestPublication(2,K::Image,0x1080,0x10a0,8));
    assert(ledger.ConfirmGuestPublication(1,K::Buffer,0x1090,0x10d0,9));
    auto snapshot{ledger.Cells()};
    assert(ledger.Join(std::vector<P>{image,buffer}));
    assert(SameCells(snapshot,ledger.Cells()));
    P pending{3,K::Image,{{0x1100,0x1120}},true};
    assert(!ledger.Join(std::vector<P>{buffer,image,pending}));
    assert(!ledger.Join(std::vector<P>{image})); // cannot discard old writer
    P bad{4,K::Image,{{0x10e0,0x10c0}}};
    assert(!ledger.Join(std::vector<P>{buffer,image,bad}));
    P duplicate{1,K::Image,{{0x1000,0x1080}}};
    assert(!ledger.Join(std::vector<P>{buffer,image,duplicate}));
    assert(!ledger.RecordWrite(1,K::Buffer,0x1000,0x1201,11));
    assert(!ledger.RecordWrite(1,K::Buffer,0x1000,0x1200,7));
    assert(!ledger.ConfirmGuestPublication(1,K::Buffer,0x1080,0x10a0,7));
    assert(SameCells(snapshot,ledger.Cells())); // every rejection is atomic
    std::puts("PASS guest-bridge transactional B->image->B writer ledger");
}
