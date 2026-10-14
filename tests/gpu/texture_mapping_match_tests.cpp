// SPDX-License-Identifier: MPL-2.0
// Small, CPU-only regression tests for physical -> logical submapping identity.
#include <gpu/texture/mapping_match.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

int main() {
    using skyline::gpu::texture::FindContainedMappingOffset;
    std::vector<std::uint8_t> memory(1024 * 1024);
    auto *p = memory.data();
    using Bytes = std::span<std::uint8_t>;

    // Matching physical spans can have a different segmentation on either
    // side; a valid non-zero mip also works when its bytes live in mapping 1.
    std::vector<Bytes> host{{p, 65536}, {p + 200000, 24576}};
    assert(FindContainedMappingOffset(host, std::vector<Bytes>{{p + 200000, 16384}}, 1) == 65536);
    assert(FindContainedMappingOffset(host, std::vector<Bytes>{{p + 65536 - 1024, 1024}, {p + 200000, 512}}, 0) == 64512);
    assert(FindContainedMappingOffset(host, std::vector<Bytes>{{p + 200000, 8192}, {p + 208192, 8192}}, 1) == 65536);

    // R1: a first mapping within the parent does not justify reusing the
    // backing if a later requested mapping points to different physical bytes.
    std::vector<Bytes> contiguousParent{{p, 90112}};
    assert(!FindContainedMappingOffset(contiguousParent,
        std::vector<Bytes>{{p + 65536, 4096}, {p + 500000, 12288}}, 0));

    // R3: physically distant mapping 1 starts at logical offset 65536, NOT
    // the numeric difference from the parent's first pointer (86016).
    std::vector<Bytes> fragmented{{p, 65536}, {p + 86016, 24576}};
    assert(FindContainedMappingOffset(fragmented,
        std::vector<Bytes>{{p + 86016, 1024}}, 1) == 65536);

    // Split at the host mapping boundary, but keep guest as a single span.
    std::vector<Bytes> continuous{{p, 128}, {p + 128, 128}};
    assert(FindContainedMappingOffset(continuous,
        std::vector<Bytes>{{p + 96, 96}}, 0) == 96);
    assert(!FindContainedMappingOffset(continuous,
        std::vector<Bytes>{{p + 96, 256}}, 0));
    assert(!FindContainedMappingOffset(continuous, std::vector<Bytes>{}, 0));
    assert(!FindContainedMappingOffset(continuous, std::vector<Bytes>{{p, 4}}, 5));
    assert(!FindContainedMappingOffset(continuous, std::vector<Bytes>{{p, 0}}, 0));

    std::puts("PASS: Texman physical mapping sequence and logical offset regression tests");
}
