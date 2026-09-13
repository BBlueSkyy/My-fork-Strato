// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "common.h"

namespace skyline::input {
    union MouseButton {
        u32 raw{};
        struct {
            u32 left : 1;
            u32 right : 1;
            u32 middle : 1;
            u32 forward : 1;
            u32 back : 1;
        };
    };
    static_assert(sizeof(MouseButton) == 0x4);

    union MouseAttribute {
        u32 raw{};
        struct {
            u32 transferable : 1;
            u32 connected : 1;
        };
    };
    static_assert(sizeof(MouseAttribute) == 0x4);

    /**
     * @url https://switchbrew.org/wiki/HID_Shared_Memory#MouseState
     */
    struct MouseState {
        u64 globalTimestamp; //!< The global timestamp in samples
        u64 localTimestamp; //!< The local timestamp in samples

        i32 positionX; //!< The X position of the mouse
        i32 positionY; //!< The Y position of the mouse

        i32 deltaX; //!< The change in the X-axis value
        i32 deltaY; //!< The change in the Y-axis value

        i32 scrollChangeY; //!< The amount scrolled in the Y-axis since the last entry
        i32 scrollChangeX; //!< The amount scrolled in the X-axis since the last entry

        MouseButton buttons;
        MouseAttribute attributes;
    };
    static_assert(sizeof(MouseState) == 0x30);
    static_assert(alignof(MouseState) == 0x8);
    static_assert(offsetof(MouseState, localTimestamp) == 0x8);
    static_assert(offsetof(MouseState, buttons) == 0x28);
    static_assert(offsetof(MouseState, attributes) == 0x2C);

    /**
     * @url https://switchbrew.org/wiki/HID_Shared_Memory#Mouse
     */
    struct MouseSection {
        CommonHeader header;
        std::array<MouseState, constant::HidEntryCount> entries;
        u64 _pad_[0x16];
    };
    static_assert(sizeof(MouseSection) == 0x400);
}
