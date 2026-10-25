// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace skyline::gpu::interconnect {
    constexpr bool SparseMipViewFits(uint32_t levels, uint32_t base, uint32_t count) {
        return count && base < levels && count <= levels - base;
    }

    // Logical texel bytes only. Optimal-tiled Vulkan memory requirements must
    // also fit the budget before allocating an image (padding is driver-specific).
    inline std::optional<size_t> SparseRgba8MipBytes(uint32_t width, uint32_t height, uint32_t levels) {
        if (!width || !height || !levels || levels > static_cast<uint32_t>(std::bit_width(std::max(width, height))))
            return std::nullopt;

        constexpr auto Max{std::numeric_limits<size_t>::max()};
        size_t total{};
        for (uint32_t level{}; level < levels; level++) {
            if (size_t{width} > Max / 4 / height)
                return std::nullopt;
            const size_t bytes{size_t{width} * height * 4};
            if (bytes > Max - total)
                return std::nullopt;
            total += bytes;
            width = std::max(1U, width >> 1);
            height = std::max(1U, height >> 1);
        }
        return total;
    }
}
