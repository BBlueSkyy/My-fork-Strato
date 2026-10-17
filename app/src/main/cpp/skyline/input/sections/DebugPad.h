// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <cstddef>
#include "common.h"

namespace skyline::input {
    /**
     * @url https://switchbrew.org/wiki/HID_Shared_Memory#DebugPadState
     */
    struct DebugPadState {
        u64 globalTimestamp; //!< Atomic storage marker (outside the SDK payload)
        u64 localTimestamp; //!< Sampling number
        u32 attributes; //!< Bit 0: connected
        u32 buttons;
        i32 rightX;
        i32 rightY;
        i32 leftX;
        i32 leftY;
    };
    static_assert(sizeof(DebugPadState) == 0x28);
    static_assert(offsetof(DebugPadState, localTimestamp) == 0x8);
    static_assert(offsetof(DebugPadState, attributes) == 0x10);
    static_assert(offsetof(DebugPadState, rightX) == 0x18);
    static_assert(offsetof(DebugPadState, leftX) == 0x20);

    /**
     * @url https://switchbrew.org/wiki/HID_Shared_Memory#DebugPad
     */
    struct DebugPadSection {
        CommonHeader header;
        std::array<DebugPadState, constant::HidEntryCount> entries;
        u64 _pad_[0x27];
    };
    static_assert(sizeof(DebugPadSection) == 0x400);
}
