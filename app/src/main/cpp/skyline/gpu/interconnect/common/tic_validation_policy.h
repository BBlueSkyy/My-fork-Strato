// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace skyline::gpu::interconnect {
    // The Maxwell TIC texture-type field is four bits wide, but only 0..8
    // describe supported texture targets. Values 9..15 are reserved.
    constexpr bool IsValidTicTextureType(std::uint32_t type) noexcept {
        return type <= 8;
    }

    // Do not interpret an uninitialized or nonresident TIC as a real image.
    // A reserved type pointing into resident GPU memory is NOT safely null:
    // preserve the hard error so that descriptor corruption is diagnosed.
    constexpr bool ShouldNullInvalidTic(std::uint32_t type, bool resident) noexcept {
        return !IsValidTicTextureType(type) && !resident;
    }
}
