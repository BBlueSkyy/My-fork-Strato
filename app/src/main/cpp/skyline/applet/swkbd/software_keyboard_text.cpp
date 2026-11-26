// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#include "software_keyboard_text.h"

namespace skyline::applet::swkbd {
    namespace {
        constexpr bool IsHighSurrogate(char16_t character) {
            return character >= 0xD800 && character <= 0xDBFF;
        }

        constexpr bool IsLowSurrogate(char16_t character) {
            return character >= 0xDC00 && character <= 0xDFFF;
        }

        bool ReadCodePoint(std::u16string_view text, size_t offset, char32_t &codePoint, size_t &units) {
            const char16_t first{text[offset]};
            if (IsHighSurrogate(first)) {
                if (offset + 1 >= text.size() || !IsLowSurrogate(text[offset + 1]))
                    return false;
                codePoint = 0x10000 + ((static_cast<char32_t>(first) - 0xD800) << 10) +
                            (static_cast<char32_t>(text[offset + 1]) - 0xDC00);
                units = 2;
                return true;
            }
            if (IsLowSurrogate(first))
                return false;
            codePoint = first;
            units = 1;
            return true;
        }

        size_t Utf8Length(char32_t codePoint) {
            if (codePoint <= 0x7F)
                return 1;
            if (codePoint <= 0x7FF)
                return 2;
            if (codePoint <= 0xFFFF)
                return 3;
            return 4;
        }

        void WriteUtf8(u8 *output, char32_t codePoint, size_t length) {
            switch (length) {
                case 1:
                    output[0] = static_cast<u8>(codePoint);
                    break;
                case 2:
                    output[0] = static_cast<u8>(0xC0 | (codePoint >> 6));
                    output[1] = static_cast<u8>(0x80 | (codePoint & 0x3F));
                    break;
                case 3:
                    output[0] = static_cast<u8>(0xE0 | (codePoint >> 12));
                    output[1] = static_cast<u8>(0x80 | ((codePoint >> 6) & 0x3F));
                    output[2] = static_cast<u8>(0x80 | (codePoint & 0x3F));
                    break;
                default:
                    output[0] = static_cast<u8>(0xF0 | (codePoint >> 18));
                    output[1] = static_cast<u8>(0x80 | ((codePoint >> 12) & 0x3F));
                    output[2] = static_cast<u8>(0x80 | ((codePoint >> 6) & 0x3F));
                    output[3] = static_cast<u8>(0x80 | (codePoint & 0x3F));
                    break;
            }
        }
    }

    TextWriteResult WriteText(span<u8> output, std::u16string_view text, TextEncoding encoding) {
        std::fill(output.begin(), output.end(), u8{});

        TextWriteResult result{};
        for (size_t offset{}; offset < text.size();) {
            char32_t codePoint{};
            size_t units{};
            if (!ReadCodePoint(text, offset, codePoint, units)) {
                result.valid = false;
                return result;
            }

            if (encoding == TextEncoding::Utf16) {
                const size_t bytes{units * sizeof(char16_t)};
                if (result.bytesWritten + bytes > output.size()) {
                    result.truncated = true;
                    return result;
                }
                for (size_t index{}; index < units; ++index) {
                    const auto character{static_cast<u16>(text[offset + index])};
                    output[result.bytesWritten++] = static_cast<u8>(character & 0xFF);
                    output[result.bytesWritten++] = static_cast<u8>(character >> 8);
                }
            } else {
                const size_t bytes{Utf8Length(codePoint)};
                if (result.bytesWritten + bytes > output.size()) {
                    result.truncated = true;
                    return result;
                }
                WriteUtf8(output.data() + result.bytesWritten, codePoint, bytes);
                result.bytesWritten += bytes;
            }

            result.codeUnitsConsumed += units;
            offset += units;
        }
        return result;
    }

    std::optional<std::u16string> ReadUtf16Text(span<const u8> input, size_t byteOffset, size_t codeUnits) {
        if ((byteOffset & 1) != 0 || byteOffset > input.size() || codeUnits > (input.size() - byteOffset) / sizeof(char16_t))
            return std::nullopt;

        std::u16string text;
        text.reserve(codeUnits);
        for (size_t index{}; index < codeUnits; ++index) {
            const size_t offset{byteOffset + index * sizeof(char16_t)};
            text.push_back(static_cast<char16_t>(static_cast<u16>(input[offset]) |
                                                 (static_cast<u16>(input[offset + 1]) << 8)));
        }
        for (size_t offset{}; offset < text.size();) {
            char32_t codePoint{};
            size_t units{};
            if (!ReadCodePoint(text, offset, codePoint, units))
                return std::nullopt;
            offset += units;
        }
        return text;
    }

    std::optional<std::u16string> ReadNullTerminatedText(span<const u8> input, TextEncoding encoding) {
        if (encoding == TextEncoding::Utf16) {
            if ((input.size() & 1) != 0)
                return std::nullopt;
            size_t units{};
            while (units < input.size() / sizeof(char16_t)) {
                const size_t offset{units * sizeof(char16_t)};
                if (input[offset] == 0 && input[offset + 1] == 0)
                    break;
                ++units;
            }
            return ReadUtf16Text(input, 0, units);
        }

        std::u16string text;
        for (size_t offset{}; offset < input.size() && input[offset] != 0;) {
            const u8 first{input[offset]};
            size_t length{};
            char32_t codePoint{};
            if (first < 0x80) {
                length = 1;
                codePoint = first;
            } else if ((first & 0xE0) == 0xC0) {
                length = 2;
                codePoint = first & 0x1F;
            } else if ((first & 0xF0) == 0xE0) {
                length = 3;
                codePoint = first & 0x0F;
            } else if ((first & 0xF8) == 0xF0) {
                length = 4;
                codePoint = first & 0x07;
            } else {
                return std::nullopt;
            }
            if (offset + length > input.size())
                return std::nullopt;
            for (size_t index{1}; index < length; ++index) {
                if ((input[offset + index] & 0xC0) != 0x80)
                    return std::nullopt;
                codePoint = (codePoint << 6) | (input[offset + index] & 0x3F);
            }
            if ((length == 2 && codePoint < 0x80) || (length == 3 && codePoint < 0x800) ||
                (length == 4 && codePoint < 0x10000) || codePoint > 0x10FFFF ||
                (codePoint >= 0xD800 && codePoint <= 0xDFFF))
                return std::nullopt;
            if (codePoint <= 0xFFFF) {
                text.push_back(static_cast<char16_t>(codePoint));
            } else {
                codePoint -= 0x10000;
                text.push_back(static_cast<char16_t>(0xD800 + (codePoint >> 10)));
                text.push_back(static_cast<char16_t>(0xDC00 + (codePoint & 0x3FF)));
            }
            offset += length;
        }
        return text;
    }
}
