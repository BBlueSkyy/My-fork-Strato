// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>
#include <gpu/interconnect/common/spirv_subgroup_probe.h>

using skyline::gpu::diagnostics::ReplaceNativeShufflesWithLocalCopies;
static constexpr std::uint32_t Magic{0x07230203};
static constexpr std::uint32_t CapShuffle{(2u << 16) | 17u};
static constexpr std::uint32_t Shuffle{(6u << 16) | 345u};
static constexpr std::uint32_t Copy{(4u << 16) | 83u};

static std::vector<std::uint32_t> Module() {
    // Five-word module header, a regular capability, a shuffle capability,
    // two native shuffles with distinct result IDs and source lanes, and a
    // trailing instruction that must be preserved byte-for-byte.
    return {Magic, 0x00010400, 0, 64, 0,
            CapShuffle, 1,
            CapShuffle, 65,
            Shuffle, 7, 8, 2, 9, 10,
            Shuffle, 7, 11, 2, 12, 13,
            (2u << 16) | 248u, 15u};
}

int main() {
    const auto original{Module()};
    auto patched{ReplaceNativeShufflesWithLocalCopies(original)};
    assert(patched.has_value());
    const std::vector<std::uint32_t> expected{
        Magic, 0x00010400, 0, 64, 0,
        CapShuffle, 1,
        Copy, 7, 8, 9,
        Copy, 7, 11, 12,
        (2u << 16) | 248u, 15u,
    };
    assert(*patched == expected);
    // No source input, result type, result IDs or other instruction changed.
    assert(original == Module());
    // Repeating the probe cannot silently alter an already-rewritten shader.
    assert(!ReplaceNativeShufflesWithLocalCopies(*patched).has_value());
    // The rewrite is never a generic SPIR-V optimizer.
    auto malformed{original};
    malformed[9] = 0;
    assert(!ReplaceNativeShufflesWithLocalCopies(malformed).has_value());
    auto wrongVersion{original};
    wrongVersion[0] = 0;
    assert(!ReplaceNativeShufflesWithLocalCopies(wrongVersion).has_value());
    auto incomplete{original};
    incomplete.resize(incomplete.size() - 6);
    assert(!ReplaceNativeShufflesWithLocalCopies(incomplete).has_value());
}
