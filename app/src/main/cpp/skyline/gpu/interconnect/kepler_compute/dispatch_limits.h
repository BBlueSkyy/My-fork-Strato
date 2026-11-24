// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>

namespace skyline::gpu::interconnect::kepler_compute {
    // Zero-sized dispatches are legal in Vulkan. Only excessive counts are
    // invalid; never clamp a guest-provided grid to the physical-device limits.
    constexpr bool IsComputeGridWithinLimits(const std::array<std::uint32_t, 3> &grid,
                                             const std::array<std::uint32_t, 3> &limits) noexcept {
        return grid[0] <= limits[0] && grid[1] <= limits[1] && grid[2] <= limits[2];
    }
}
