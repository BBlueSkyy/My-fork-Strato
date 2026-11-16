// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "nacp.h"
#include <common/utf8.h>
#include <zlib.h>

namespace skyline::vfs {
    namespace {
        template<size_t Size>
        std::string ReadText(const std::array<char, Size> &field) {
            const auto end{std::find(field.begin(), field.end(), '\0')};
            if (end == field.end())
                throw exception("Unterminated NACP text field");
            std::string text(field.begin(), end);
            util::Utf8ToUtf16(text);
            return text;
        }
    }

    NACP::NACP(const std::shared_ptr<vfs::Backing> &backing) {
        if (!backing || backing->size < sizeof(nacpContents) ||
            backing->Read(span<u8>(reinterpret_cast<u8 *>(&nacpContents), sizeof(nacpContents))) != sizeof(nacpContents))
            throw exception("Missing or truncated NACP");

        if (nacpContents.titlesDataFormat == 0) {
            std::copy(nacpContents.titleEntries.begin(), nacpContents.titleEntries.end(), titleEntries.begin());
        } else if (nacpContents.titlesDataFormat == 1) {
            // Only the 0x3000-byte title area is compressed. Its first two bytes
            // contain the little-endian payload size; the remaining NACP stays raw.
            const auto data{reinterpret_cast<const u8 *>(nacpContents.titleEntries.data())};
            const size_t compressedSize{static_cast<size_t>(data[0]) | (static_cast<size_t>(data[1]) << 8)};
            if (compressedSize == 0 || compressedSize > sizeof(nacpContents.titleEntries) - 2)
                throw exception("Invalid compressed NACP title size");

            z_stream stream{};
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
                throw exception("Failed to initialize NACP title decompression");
            stream.next_in = const_cast<Bytef *>(data + 2);
            stream.avail_in = static_cast<uInt>(compressedSize);
            stream.next_out = reinterpret_cast<Bytef *>(titleEntries.data());
            stream.avail_out = static_cast<uInt>(sizeof(titleEntries));
            const auto result{inflate(&stream, Z_FINISH)};
            const bool complete{result == Z_STREAM_END && stream.total_in == compressedSize && stream.total_out == sizeof(titleEntries)};
            inflateEnd(&stream);
            if (!complete)
                throw exception("Invalid or incomplete compressed NACP titles");
        } else {
            throw exception("Unsupported NACP title data format: {}", nacpContents.titlesDataFormat);
        }

        u32 counter{};
        for (const auto &entry : titleEntries) {
            if (!ReadText(entry.applicationName).empty())
                supportedTitleLanguages |= (u32{1} << counter);
            ReadText(entry.applicationPublisher);
            counter++;
        }
        ReadText(nacpContents.displayVersion);
    }

    language::ApplicationLanguage NACP::GetFirstSupportedTitleLanguage() {
        if (supportedTitleLanguages == 0)
            throw exception("NACP has no available title language");
        return static_cast<language::ApplicationLanguage>(std::countr_zero(supportedTitleLanguages));
    }

    language::ApplicationLanguage NACP::GetFirstSupportedLanguage() {
        if (nacpContents.supportedLanguageFlag == 0)
            throw exception("NACP has no supported application language");
        return static_cast<language::ApplicationLanguage>(std::countr_zero(nacpContents.supportedLanguageFlag));
    }

    std::string NACP::GetApplicationName(language::ApplicationLanguage language) {
        return ReadText(titleEntries.at(static_cast<size_t>(language)).applicationName);
    }

    std::string NACP::GetApplicationVersion() {
        return ReadText(nacpContents.displayVersion);
    }

    std::string NACP::GetAddOnContentBaseId() {
        auto addOnContentBaseId{nacpContents.addOnContentBaseId};
        return fmt::format("{:016X}", addOnContentBaseId);
    }

    std::string NACP::GetSaveDataOwnerId() {
        auto applicationTitleId{nacpContents.saveDataOwnerId};
        return fmt::format("{:016X}", applicationTitleId);
    }

    std::string NACP::GetApplicationPublisher(language::ApplicationLanguage language) {
        return ReadText(titleEntries.at(static_cast<size_t>(language)).applicationPublisher);
    }
}
