// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <vector>
#include "software_keyboard_text.h"

namespace skyline::applet::swkbd {
    enum class InlineReply : u32 {
        FinishedInitialize = 0x0,
        Default = 0x1,
        ChangedString = 0x2,
        MovedCursor = 0x3,
        DecidedEnter = 0x5,
        DecidedCancel = 0x6,
        ChangedStringUtf8 = 0x7,
        MovedCursorUtf8 = 0x8,
        DecidedEnterUtf8 = 0x9,
        UnsetCustomizeDictionary = 0xA,
        ReleasedUserWordInfo = 0xB,
        UnsetCustomizedDictionaries = 0xC,
        ChangedStringV2 = 0xD,
        MovedCursorV2 = 0xE,
        ChangedStringUtf8V2 = 0xF,
        MovedCursorUtf8V2 = 0x10,
    };

    /** Builds the fixed-size inline reply, keeping cursor arguments outside the text buffer. */
    std::vector<u8> BuildInlineTextReply(u32 state, InlineReply reply, std::u16string_view text, i32 cursor);
}
