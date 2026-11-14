// SPDX-License-Identifier: MPL-2.0
// Standalone, executable contract for the temporary TIC probe helpers.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <gpu/interconnect/common/tic_probe.h>

namespace probe = skyline::gpu::interconnect::tic_probe;

struct MockTic {
    std::uint32_t words[8];
};
static_assert(sizeof(MockTic) == 32);

int main() {
    MockTic tic{{0x01234567, 0x89ABCDEF, 2, 3, 4, 5, 6, 0xFF00EE11}};
    const auto snapshot{probe::SnapshotWords(tic)};
    for (std::uint32_t i = 0; i < 8; ++i)
        assert(snapshot[i] == tic.words[i]);
    tic.words[0] = 999;
    assert(snapshot[0] == 0x01234567); // detached snapshot, not a guest pointer

    assert(probe::currentShader.hash == 0);
    {
        probe::ScopedShaderContext graphics{"graphics", 5, 0xABCDE};
        assert(probe::currentShader.stage == 5);
        assert(probe::currentShader.hash == 0xABCDE);
        {
            probe::ScopedShaderContext compute{"compute", 0, 0x222};
            assert(probe::currentShader.hash == 0x222);
        }
        assert(probe::currentShader.hash == 0xABCDE);
    }
    assert(probe::currentShader.hash == 0);

    assert(!probe::ConsumeRecovered(0x1000, 7));
    probe::RememberReserved(0x1000, 7);
    probe::RememberReserved(0x1000, 7);
    assert(!probe::ConsumeRecovered(0x2000, 7));
    assert(!probe::ConsumeRecovered(0x1000, 8));
    assert(probe::ConsumeRecovered(0x1000, 7));
    assert(!probe::ConsumeRecovered(0x1000, 7));

    const auto key{probe::Fingerprint(0x1000, 7, snapshot, 0xABCDE, 2)};
    assert(key != probe::Fingerprint(0x1000, 8, snapshot, 0xABCDE, 2));
    assert(key != probe::Fingerprint(0x1000, 7, snapshot, 0xABCDE, 1));
    assert(probe::ShouldEmit(key));
    assert(!probe::ShouldEmit(key));
    assert(probe::ShouldEmit(probe::Fingerprint(0x1000, 8, snapshot, 0xABCDE, 2)));

    probe::WatchFirstTic(0x54A070000ULL);
    assert(probe::OverlapsWatchedFirstTic(0x54A070000ULL, 32));
    assert(probe::OverlapsWatchedFirstTic(0x54A06FFF0ULL, 16 + 1));
    assert(probe::OverlapsWatchedFirstTic(0x54A07001FULL, 1));
    assert(!probe::OverlapsWatchedFirstTic(0x54A070020ULL, 32));
    assert(!probe::OverlapsWatchedFirstTic(0x54A06FFF0ULL, 16));
    assert(!probe::OverlapsWatchedFirstTic(0x54A070000ULL, 0));
    assert(!probe::Overlaps(0xFFFFFFFFFFFFFFFEULL, 2, 0x10ULL));
    assert(probe::SuspectDword == 0x7FBFFFFF);
    assert(probe::ShouldEmitWriter());
    probe::WatchFirstTic(0);
    assert(!probe::OverlapsWatchedFirstTic(0x54A070000ULL, 32));

    // Only associate an NvMap record with a live GMMU lookup when both the
    // IOVA range and the translated host backing byte match exactly.
    int asA{};
    int asB{};
    std::array<std::uint8_t, 128> guestBacking{};
    std::array<std::uint8_t, 128> replacementBacking{};
    const auto physA{reinterpret_cast<std::uintptr_t>(guestBacking.data())};
    const auto physB{reinterpret_cast<std::uintptr_t>(replacementBacking.data())};
    probe::RememberMapping(&asA, 0x54000000, 128, physA, 0x72000000, 24,
                           probe::MappingSource::Fixed);
    assert(!probe::FindMapping(&asB, 0x54000020, physA + 32));
    assert(!probe::FindMapping(&asA, 0x54000100, physA));
    assert(!probe::FindMapping(&asA, 0x54000020, physA + 16));
    const auto mapped{probe::FindMapping(&asA, 0x54000020, physA + 32)};
    assert(mapped && mapped->nvMapId == 24);
    assert(mapped->nvMapBackingBase + (0x54000020 - mapped->gpuBase) == 0x72000020);
    // A new mapping supersedes the old GPU IOVA, but a matching host backing
    // is still required: old records cannot masquerade as the new backing.
    probe::RememberMapping(&asA, 0x54000020, 32, physB, 0x73000000, 25,
                           probe::MappingSource::SubregionRemap);
    const auto remapped{probe::FindMapping(&asA, 0x54000024, physB + 4)};
    assert(remapped && remapped->nvMapId == 25);
    assert(remapped->source == probe::MappingSource::SubregionRemap);
    assert(!probe::FindMapping(&asA, 0x54000024, physB + 8));
    probe::ForgetMappingRange(&asA, 0x54000000, 128);
    assert(!probe::FindMapping(&asA, 0x54000020, physA + 32));
    assert(!probe::FindMapping(&asA, 0x54000024, physB + 4));

    std::puts("TIC probe snapshot/context/transition/writer/NvMap: PASS");
}
