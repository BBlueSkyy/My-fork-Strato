// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstdint>

namespace skyline::gpu::blit {
    constexpr std::uint32_t MipDimension(std::uint32_t base, std::uint32_t mip) {
        return mip < 32 ? std::max(1U, base >> mip) : 1U;
    }

    struct NormalizedAxis {
        float origin, span;
    };

    // Vulkan UVs address the actual selected mip, even when a padded guest surface
    // resolves to an existing image with a different logical extent.
    constexpr NormalizedAxis NormalizeAxis(float origin, float span, std::uint32_t backingDimension) {
        return {origin / backingDimension, span / backingDimension};
    }
}
