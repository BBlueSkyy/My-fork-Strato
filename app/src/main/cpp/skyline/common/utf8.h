// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace skyline::util {
    /**
     * @brief Decodes standard UTF-8 without replacing malformed input. JNI NewStringUTF
     * accepts Modified UTF-8 instead; use the UTF-16 result with JNI NewString.
     */
    inline std::u16string Utf8ToUtf16(std::string_view input) {
        std::u16string output;
        output.reserve(input.size());
        for (size_t offset{}; offset < input.size();) {
            const auto first{static_cast<uint8_t>(input[offset++])};
            uint32_t codePoint{};
            uint32_t minimum{};
            size_t continuationBytes{};
            if (first < 0x80) {
                codePoint = first;
            } else if (first >= 0xC2 && first <= 0xDF) {
                codePoint = first & 0x1F;
                minimum = 0x80;
                continuationBytes = 1;
            } else if (first >= 0xE0 && first <= 0xEF) {
                codePoint = first & 0x0F;
                minimum = 0x800;
                continuationBytes = 2;
            } else if (first >= 0xF0 && first <= 0xF4) {
                codePoint = first & 0x07;
                minimum = 0x10000;
                continuationBytes = 3;
            } else {
                throw std::invalid_argument("Invalid UTF-8 leading byte");
            }

            if (continuationBytes > input.size() - offset)
                throw std::invalid_argument("Truncated UTF-8 sequence");
            for (size_t index{}; index < continuationBytes; ++index) {
                const auto next{static_cast<uint8_t>(input[offset++])};
                if ((next & 0xC0) != 0x80)
                    throw std::invalid_argument("Invalid UTF-8 continuation byte");
                codePoint = (codePoint << 6) | (next & 0x3F);
            }
            if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
                throw std::invalid_argument("Invalid UTF-8 code point");

            if (codePoint <= 0xFFFF) {
                output.push_back(static_cast<char16_t>(codePoint));
            } else {
                codePoint -= 0x10000;
                output.push_back(static_cast<char16_t>(0xD800 + (codePoint >> 10)));
                output.push_back(static_cast<char16_t>(0xDC00 + (codePoint & 0x3FF)));
            }
        }
        return output;
    }
}
