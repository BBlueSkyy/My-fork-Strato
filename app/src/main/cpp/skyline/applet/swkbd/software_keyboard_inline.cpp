// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#include <cstring>
#include "software_keyboard_inline.h"

namespace skyline::applet::swkbd {
    std::vector<u8> BuildInlineTextReply(u32 state, InlineReply reply, std::u16string_view text, i32 cursor) {
        const bool utf8{reply == InlineReply::ChangedStringUtf8 || reply == InlineReply::ChangedStringUtf8V2 ||
                        reply == InlineReply::MovedCursorUtf8 || reply == InlineReply::MovedCursorUtf8V2 ||
                        reply == InlineReply::DecidedEnterUtf8};
        const bool changed{reply == InlineReply::ChangedString || reply == InlineReply::ChangedStringV2 ||
                           reply == InlineReply::ChangedStringUtf8 || reply == InlineReply::ChangedStringUtf8V2};
        const bool moved{reply == InlineReply::MovedCursor || reply == InlineReply::MovedCursorV2 ||
                         reply == InlineReply::MovedCursorUtf8 || reply == InlineReply::MovedCursorUtf8V2};
        const bool decidedEnter{reply == InlineReply::DecidedEnter || reply == InlineReply::DecidedEnterUtf8};
        const bool version2{reply == InlineReply::ChangedStringV2 || reply == InlineReply::ChangedStringUtf8V2 ||
                            reply == InlineReply::MovedCursorV2 || reply == InlineReply::MovedCursorUtf8V2};
        if (!changed && !moved && !decidedEnter)
            throw exception("Invalid software keyboard inline text reply");

        constexpr size_t HeaderBytes{sizeof(u32) * 2};
        const size_t textBytes{utf8 ? 0x7D4U : 0x3ECU};
        const size_t argumentBytes{sizeof(u32) * (changed ? 4 : moved ? 2 : 1)};
        std::vector<u8> response(HeaderBytes + textBytes + argumentBytes + (version2 ? 1 : 0));
        auto write = [&](size_t offset, auto value) { std::memcpy(response.data() + offset, &value, sizeof(value)); };
        write(0, state);
        write(sizeof(u32), reply);
        WriteText({response.data() + HeaderBytes, textBytes}, text, utf8 ? TextEncoding::Utf8 : TextEncoding::Utf16);

        const size_t argumentOffset{HeaderBytes + textBytes};
        write(argumentOffset, static_cast<u32>(text.size()));
        if (changed) {
            write(argumentOffset + sizeof(u32), static_cast<i32>(-1));
            write(argumentOffset + sizeof(u32) * 2, static_cast<i32>(-1));
        }
        if (changed || moved)
            write(argumentOffset + argumentBytes - sizeof(u32), std::clamp(cursor, 0, static_cast<i32>(text.size())));
        return response;
    }
}
