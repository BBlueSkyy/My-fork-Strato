// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <optional>
#include <string_view>
#include <common.h>

namespace skyline::applet::swkbd {
    enum class TextEncoding {
        Utf8,
        Utf16,
    };

    struct TextWriteResult {
        size_t bytesWritten{};
        size_t codeUnitsConsumed{};
        bool truncated{};
        bool valid{true};
    };

    TextWriteResult WriteText(span<u8> output, std::u16string_view text, TextEncoding encoding);

    std::optional<std::u16string> ReadUtf16Text(span<const u8> input, size_t byteOffset, size_t codeUnits);

    std::optional<std::u16string> ReadNullTerminatedText(span<const u8> input, TextEncoding encoding);
}