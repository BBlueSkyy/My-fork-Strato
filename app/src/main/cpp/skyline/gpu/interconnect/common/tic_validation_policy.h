// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace skyline::gpu::interconnect {
    // The Maxwell TIC texture-type field is four bits wide, but only 0..8
    // describe supported texture targets. Values 9..15 are reserved.
    constexpr bool IsValidTicTextureType(std::uint32_t type) noexcept {
        return type <= 8;
    }

    // Compatibility policy for sampled images only; not a hardware guarantee.
    // Missing base-address backing does not establish an image extent or make
    // storage writes safe to discard. Callers must preserve InvalidDescriptor.
    // A mapped reserved target remains fatal to diagnose descriptor corruption.
    constexpr bool ShouldNullInvalidTic(std::uint32_t type, bool resident) noexcept {
        return !IsValidTicTextureType(type) && !resident;
    }
}
