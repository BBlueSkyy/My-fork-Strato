// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#include "cnmt.h"

namespace skyline::vfs {
    namespace {
        template<typename T>
        T ReadCnmt(const std::shared_ptr<Backing> &backing, size_t offset = 0) {
            T value{};
            if (backing->Read(span<u8>(reinterpret_cast<u8 *>(&value), sizeof(value)), offset) != sizeof(value))
                throw exception("Truncated CNMT metadata");
            return value;
        }
    }

    CNMT::CNMT(std::shared_ptr<FileSystem> cnmtSection) {
        if (!cnmtSection)
            throw exception("Missing CNMT partition");
        auto root{cnmtSection->OpenDirectory("")};
        std::shared_ptr<vfs::Backing> cnmt;
        if (root != nullptr) {
            for (const auto &entry : root->Read()) {
                if (!entry.name.ends_with(".cnmt"))
                    continue;
                if (cnmt)
                    throw exception("Ambiguous CNMT partition");
                cnmt = cnmtSection->OpenFile(entry.name);
            }
        }

        if (!cnmt)
            throw exception("Missing CNMT file");
        header = ReadCnmt<PackagedContentMetaHeader>(cnmt);
        const size_t contentStart{sizeof(PackagedContentMetaHeader) + header.extendedHeaderSize};
        const size_t metaStart{contentStart + header.contentCount * sizeof(PackagedContentInfo)};
        if (metaStart + header.contentMetaCount * sizeof(ContentMetaInfo) > cnmt->size)
            throw exception("CNMT records exceed the file size");
        if (header.contentMetaType >= ContentMetaType::Application && header.contentMetaType <= ContentMetaType::AddOnContent) {
            if (header.extendedHeaderSize < sizeof(OptionalHeader))
                throw exception("Truncated application/patch CNMT extended header");
            optionalHeader = ReadCnmt<OptionalHeader>(cnmt, sizeof(PackagedContentMetaHeader));
        }

        for (u16 i = 0; i < header.contentCount; ++i)
            contentInfos.emplace_back(ReadCnmt<PackagedContentInfo>(cnmt, contentStart + i * sizeof(PackagedContentInfo)));

        for (u16 i = 0; i < header.contentMetaCount; ++i)
            contentMetaInfos.emplace_back(ReadCnmt<ContentMetaInfo>(cnmt, metaStart + i * sizeof(ContentMetaInfo)));
    }

    std::string CNMT::GetTitleId() {
        auto tilteId{header.id};
        return fmt::format("{:016X}", tilteId);
    }

    std::string CNMT::GetParentTitleId() {
        auto parentTilteId{optionalHeader.titleId};
        return fmt::format("{:016X}", parentTilteId);
    }

    ContentMetaType CNMT::GetContentMetaType() {
        return header.contentMetaType;
    }

}
