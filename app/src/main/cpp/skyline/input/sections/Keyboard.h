// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "common.h"

namespace skyline::input {
    union KeyboardModifier {
        u32 raw{};
        struct {
            u32 control : 1;
            u32 shift : 1;
            u32 leftAlt : 1;
            u32 rightAlt : 1;
            u32 gui : 1;
            u32 _reserved0_ : 3;
            u32 capsLock : 1; //!< Caps-Lock Key
            u32 scrollLock : 1; //!< Scroll-Lock Key
            u32 numLock : 1; //!< Num-Lock Key
            u32 katakana : 1;
            u32 hiragana : 1;
        };
    };
    static_assert(sizeof(KeyboardModifier) == 0x4);

    union KeyboardAttribute {
        u32 raw{};
        struct {
            u32 connected : 1;
        };
    };
    static_assert(sizeof(KeyboardAttribute) == 0x4);

    /**
     * @url https://switchbrew.org/wiki/HID_Shared_Memory#KeyboardState
     */
    struct KeyboardState {
        u64 globalTimestamp; //!< The global timestamp in samples
        u64 localTimestamp; //!< The local timestamp in samples

        KeyboardModifier modifiers; //!< The state of any modifier keys
        KeyboardAttribute attributes;
        std::array<u8, 32> keysDown; //!< A 256-bit array indexed by USB HID usage ID
    };
    static_assert(sizeof(KeyboardState) == 0x38);
    static_assert(alignof(KeyboardState) == 0x8);
    static_assert(offsetof(KeyboardState, modifiers) == 0x10);
    static_assert(offsetof(KeyboardState, attributes) == 0x14);
    static_assert(offsetof(KeyboardState, keysDown) == 0x18);

    /**
     * @url https://switchbrew.org/wiki/HID_Shared_Memory#Keyboard
     */
    struct KeyboardSection {
        CommonHeader header;
        std::array<KeyboardState, constant::HidEntryCount> entries;
        u64 _pad_[0x5];
    };
    static_assert(sizeof(KeyboardSection) == 0x400);
}
