// SPDX-License-Identifier: MPL-2.0
// Temporary, bounded and asynchronous BOTW present-pixel diagnostic.
// Raw hashes are only comparable across frames with the same format/key.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace skyline::diag {
    inline constexpr std::uint32_t BotwPixelPatchSide{16};
    inline constexpr std::size_t BotwPixelPatchCount{4};
    inline constexpr std::size_t BotwPixelPatchBytes{BotwPixelPatchSide * BotwPixelPatchSide * 4};
    inline constexpr std::size_t BotwPixelProbeBytes{BotwPixelPatchBytes * BotwPixelPatchCount};

    struct BotwPixelRect {
        std::uint32_t x{}, y{};
    };

    inline constexpr std::array<BotwPixelRect, BotwPixelPatchCount>
    BotwPixelRects(std::uint32_t width, std::uint32_t height) {
        const auto x1{std::min(width / 4, width - BotwPixelPatchSide)};
        const auto x2{std::min((3 * width) / 4, width - BotwPixelPatchSide)};
        const auto y1{std::min(height / 4, height - BotwPixelPatchSide)};
        const auto y2{std::min((3 * height) / 4, height - BotwPixelPatchSide)};
        return {{{x1, y1}, {x2, y1}, {x1, y2}, {x2, y2}}};
    }

    inline std::array<std::uint64_t, BotwPixelPatchCount> HashBotwPixelPatches(const std::uint8_t *data) {
        std::array<std::uint64_t, BotwPixelPatchCount> hashes{};
        for (std::size_t patch{}; patch < hashes.size(); ++patch) {
            std::uint64_t hash{14695981039346656037ULL}; // FNV-1a 64-bit
            for (std::size_t byte{}; byte < BotwPixelPatchBytes; ++byte) {
                hash ^= data[patch * BotwPixelPatchBytes + byte];
                hash *= 1099511628211ULL;
            }
            hashes[patch] = hash;
        }
        return hashes;
    }

    inline std::size_t ChangedBotwPixelPatches(
        const std::array<std::uint64_t, BotwPixelPatchCount> &a,
        const std::array<std::uint64_t, BotwPixelPatchCount> &b) {
        std::size_t changes{};
        for (std::size_t i{}; i < a.size(); ++i)
            changes += a[i] != b[i];
        return changes;
    }
}
