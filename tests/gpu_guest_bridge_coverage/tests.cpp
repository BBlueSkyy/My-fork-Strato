// SPDX-License-Identifier: MPL-2.0
// Standalone byte oracle: inverse Morton addressing is deliberately independent
// from the production planner's sector-enumeration algorithm.
#include "../../app/src/main/cpp/skyline/gpu/texture/guest_bridge_coverage.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace skyline::gpu::texture;

static void Check(std::vector<GuestBridgeSurface2D> surfaces, size_t total) {
    auto result = PlanGuestBridgeByteCoverage(surfaces, total);
    assert(result && !result->empty());
    std::vector<uint8_t> expected(total), actual(total);
    for (const auto &s : surfaces) {
        for (uint64_t row{}; row < s.rows; ++row) {
            const auto gy = row / 8, y = row % 8;
            for (uint64_t x{}; x < s.widthBytes; ++x) {
                const auto gx = x / 64, sectorX = (x % 64) / 16;
                const auto sector = (y & 1) | ((sectorX & 1) << 1) |
                    ((y & 2) << 1) | ((y & 4) << 1) | ((sectorX & 2) << 3);
                const auto gob = ((gy / s.blockHeightGobs) * s.storageGobsX + gx) *
                    s.blockHeightGobs + gy % s.blockHeightGobs;
                const auto position = s.offset + gob * 512 + sector * 16 + x % 16;
                assert(position < total);
                assert(!expected[position]);
                expected[position] = 1;
            }
        }
    }
    size_t bytes{}, gaps{};
    for (const auto &run : *result) {
        assert(run.begin < run.end && run.end <= total);
        for (auto p = run.begin; p < run.end; ++p) {
            assert(!actual[p]);
            actual[p] = 1;
            ++bytes;
        }
    }
    assert(expected == actual);
    for (auto x : actual) gaps += !x;
    assert(bytes + gaps == total);
}

int main() {
    // Native formats/BC-style block widths are geometry inputs, not runtime
    // eligibility declarations: the staged bridge must still prove reversibility.
    for (uint32_t bh : {1u, 2u, 4u, 8u}) {
        for (uint32_t bytesPerBlock : {1u, 2u, 4u, 8u, 12u, 16u}) {
            for (uint32_t width : {1u, 7u, 16u, 17u, 31u, 63u, 64u, 65u, 129u}) {
                for (uint32_t rows : {1u, 7u, 8u, 9u, 16u, 33u}) {
                    const uint64_t widthBytes = uint64_t(width) * bytesPerBlock;
                    const uint64_t gobsX = (widthBytes + 63) / 64;
                    // Extra GOB columns are padding from TIC tile-width spacing.
                    const uint64_t columns = gobsX + (width % 3 == 0 ? 2 : 0);
                    const uint64_t blockRows = ((rows + 7) / 8 + bh - 1) / bh;
                    const size_t allocated = size_t(blockRows * columns * bh * 512 + 129);
                    Check({{64,widthBytes,rows,columns,bh}}, allocated);
                }
            }
        }
    }
    // A 1024x1024 RGBA8 image exceeds the direct 128-GOB transport by 64x.
    Check({{0,4096,1024,64,4}}, 4096 * 1024);
    // Mips/layers have explicit offsets and padded gaps, not one monolithic run.
    Check({{256,260,19,7,2},{32768,132,9,4,1},{65536,68,5,3,1}}, 72000);
    // A 2D 1x1 image writes 4 bytes, never claims the rest of its GOB.
    auto edge = PlanGuestBridgeByteCoverage(std::vector<GuestBridgeSurface2D>{{0,4,1,1,1}}, 512);
    assert(edge && edge->size() == 1 && (*edge)[0].begin == 0 && (*edge)[0].end == 4);
    // Negative controls: never admit out-of-bounds, zero, impossible stride,
    // overlapping mip/layer backing, work budget exhaustion, or size overflow.
    using S = GuestBridgeSurface2D;
    assert(!PlanGuestBridgeByteCoverage(std::vector<S>{{0,4,1,1,1}}, 4));
    assert(!PlanGuestBridgeByteCoverage(std::vector<S>{{0,0,1,1,1}}, 512));
    assert(!PlanGuestBridgeByteCoverage(std::vector<S>{{0,65,8,1,1}}, 1024));
    assert(!PlanGuestBridgeByteCoverage(std::vector<S>{{0,64,8,1,3}}, 512));
    assert(!PlanGuestBridgeByteCoverage(std::vector<S>{{0,64,8,1,1},{0,64,8,1,1}}, 1024));
    assert(!PlanGuestBridgeByteCoverage(std::vector<S>{{0,65,7,2,1}}, 1024, 1));
    assert(!PlanGuestBridgeByteCoverage(std::vector<S>{{0,4096,1024,64,4}}, 4096*1024, 262144, 128));
    assert(!PlanGuestBridgeByteCoverage(std::vector<S>{{UINT64_MAX - 10,64,8,1,1}}, UINT64_MAX));
    assert(!PlanGuestBridgeByteCoverage(std::vector<S>{{0,UINT64_MAX,UINT64_MAX,UINT64_MAX,1}}, UINT64_MAX));
    // The production mip/layer preflight must reject an offset that is
    // already beyond the stride before performing any unsigned subtraction.
    assert(GuestBridgeSubresourceOffset(512,128,256,1024,2048) == 640);
    assert(!GuestBridgeSubresourceOffset(512,2048,16,1024,4096));
    assert(!GuestBridgeSubresourceOffset(512,1000,40,1024,4096));
    assert(!GuestBridgeSubresourceOffset(1800,200,80,1024,2048));
    assert(!GuestBridgeSubresourceOffset(UINT64_MAX-4,8,8,16,UINT64_MAX));
    // A guest mapping can cross two unrelated physical allocations in the
    // middle of a complete GOB. The physical run must be split exactly.
    const std::vector<GuestBridgeByteRun> logical{{0,1024}};
    const std::vector<GuestBridgeMapping> mapping{{0x10000,512},{0x30000,512}};
    auto resolved = ResolveGuestBridgePhysicalRuns(logical, mapping);
    assert(resolved && resolved->size() == 2);
    assert((*resolved)[0].begin == 0x10000 && (*resolved)[0].end == 0x10200 && (*resolved)[0].guestOffset == 0);
    assert((*resolved)[1].begin == 0x30000 && (*resolved)[1].end == 0x30200 && (*resolved)[1].guestOffset == 512);
    // A contiguous physical backing may coalesce when logical offsets match.
    auto adjacent = ResolveGuestBridgePhysicalRuns(logical, std::vector<GuestBridgeMapping>{{0x10000,512},{0x10200,512}});
    assert(adjacent && adjacent->size() == 1 && (*adjacent)[0].end == 0x10400);
    // Non-monotonic physical maps must preserve logical guestOffset.
    auto reversed = ResolveGuestBridgePhysicalRuns(logical, std::vector<GuestBridgeMapping>{{0x30000,512},{0x10000,512}});
    assert(reversed && reversed->size() == 2 && (*reversed)[0].guestOffset == 512);
    // No missing mappings, physical self-aliases, duplicate logical claims,
    // overflowed physical endpoints or byte ranges outside mapped guest size.
    assert(!ResolveGuestBridgePhysicalRuns(logical, std::vector<GuestBridgeMapping>{{0x10000,512}}));
    assert(!ResolveGuestBridgePhysicalRuns(logical, std::vector<GuestBridgeMapping>{{0x10000,512},{0x10000,512}}));
    assert(!ResolveGuestBridgePhysicalRuns(std::vector<GuestBridgeByteRun>{{0,600},{599,1024}}, mapping));
    assert(!ResolveGuestBridgePhysicalRuns(std::vector<GuestBridgeByteRun>{{1000,1040}}, mapping));
    assert(!ResolveGuestBridgePhysicalRuns(logical, std::vector<GuestBridgeMapping>{{UINTPTR_MAX-511,512},{0x10000,512}}));
    assert(!ResolveGuestBridgePhysicalRuns(logical, mapping, 1));
    std::puts("PASS staged guest-bridge 2D encoder bytes, mapped physical spans, and negative admission");
}
