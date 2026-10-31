// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors
#pragma once

#include <common.h>

namespace skyline::loader {
    // Native Switch 2 application IDs use the 0x04 prefix. Switch 2 Editions
    // of Switch 1 games can retain 0x01 IDs and are deliberately NOT classified
    // here: those need content/platform identification, not a title-ID guess.
    constexpr bool IsNativeSwitch2TitleId(u64 titleId) noexcept {
        return (titleId & 0xFF00000000000000ULL) == 0x0400000000000000ULL;
    }

    // Regression guards for both architectures. The last case must never turn
    // a Switch 1 title into a Switch 2 process merely due to its lower bits.
    static_assert(IsNativeSwitch2TitleId(0x040074A01BF12000ULL)); // DK Bananza
    static_assert(!IsNativeSwitch2TitleId(0x0100000000001000ULL));
    static_assert(!IsNativeSwitch2TitleId(0));
}
