// SPDX-License-Identifier: MPL-2.0
#include <cctype>
#include "program_content.h"

namespace skyline::loader {
    ProgramNcaSelection SelectProgramNcas(std::vector<ProgramNcaCandidate> candidates, const std::vector<vfs::CNMT> &metadata,
                                          u8 programIndex) {
        ProgramNcaSelection result;
        std::array<u32, 2> versions{};
        std::array<std::string, 2> contentIds{};
        std::array<std::optional<vfs::CNMT>, 2> selectedMetadata;
        bool hasProgramRecords{};
        for (const auto &meta : metadata)
            for (const auto &record : meta.GetContentInfos())
                hasProgramRecords |= record.contentType == vfs::ContentType::Program;

        for (auto &candidate : candidates) {
            std::transform(candidate.filename.begin(), candidate.filename.end(), candidate.filename.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            bool matched{};
            for (const auto &meta : metadata) {
                if (meta.header.contentMetaType != vfs::ContentMetaType::Application && meta.header.contentMetaType != vfs::ContentMetaType::Patch)
                    continue;
                for (const auto &record : meta.GetContentInfos()) {
                    if (record.contentType != vfs::ContentType::Program || record.idOffset != programIndex)
                        continue;
                    std::string filename;
                    for (const auto byte : record.contentId)
                        filename += fmt::format("{:02x}", byte);
                    filename += ".nca";
                    if (filename != candidate.filename)
                        continue;
                    const bool patch{meta.header.contentMetaType == vfs::ContentMetaType::Patch};
                    const u64 applicationId{patch ? meta.GetParentProgramId() : meta.header.id};
                    const u64 programId{applicationId + programIndex};
                    if (candidate.nca.header.titleId != programId)
                        throw exception("CNMT Program record disagrees with NCA Program ID");
                    auto &selected{patch ? result.patch : result.base};
                    if (selected && selected->header.titleId != candidate.nca.header.titleId)
                        throw exception("Container contains multiple Program NCAs for the selected ProgramIndex");
                    if (selected && meta.header.version == versions[patch] && contentIds[patch] != filename)
                        throw exception("Ambiguous Program NCA records at the same version");
                    if (!selected || meta.header.version > versions[patch]) {
                        selected = candidate.nca;
                        versions[patch] = meta.header.version;
                        contentIds[patch] = filename;
                        selectedMetadata[patch] = meta;
                    }
                    matched = true;
                }
            }
            if (!matched && !hasProgramRecords && programIndex == 0) {
                auto &selected{candidate.nca.HasBktrSection() ? result.patch : result.base};
                if (selected)
                    throw exception("Ambiguous Program NCAs without CNMT records");
                selected = std::move(candidate.nca);
            }
        }
        // Some multi-program packages do not expose the requested ProgramIndex through
        // PackagedContentInfo::idOffset even though the Program NCA itself carries the
        // correct Program ID. HOS/Ryujinx semantics derive the program index from the
        // low nibble of the Program ID, so use that as a conservative fallback only
        // when CNMT matching selected nothing.
        if (!result.base && !result.patch) {
            std::vector<u64> applicationBases;
            for (const auto &meta : metadata) {
                if (meta.header.contentMetaType == vfs::ContentMetaType::Application)
                    applicationBases.push_back(meta.header.id & ~0xFULL);
                else if (meta.header.contentMetaType == vfs::ContentMetaType::Patch)
                    applicationBases.push_back(meta.GetParentProgramId() & ~0xFULL);
            }
            std::sort(applicationBases.begin(), applicationBases.end());
            applicationBases.erase(std::unique(applicationBases.begin(), applicationBases.end()), applicationBases.end());

            ProgramNcaCandidate *fallback{};
            for (auto &candidate : candidates) {
                if ((candidate.nca.header.titleId & 0xFULL) != programIndex)
                    continue;

                const u64 candidateBase{candidate.nca.header.titleId & ~0xFULL};
                if (!applicationBases.empty() &&
                    std::find(applicationBases.begin(), applicationBases.end(), candidateBase) == applicationBases.end())
                    continue;

                if (fallback)
                    throw exception("Ambiguous Program NCA fallback for ProgramIndex {}", programIndex);
                fallback = &candidate;
            }

            if (fallback) {
                auto &selected{fallback->nca.HasBktrSection() ? result.patch : result.base};
                selected = fallback->nca;
                LOGI("Selected ProgramIndex {} by Program NCA title ID fallback (0x{:016X})",
                     programIndex, fallback->nca.header.titleId);
            }
        }

        if (result.base && result.patch && result.base->header.titleId != result.patch->header.titleId)
            throw exception("Program patch and base belong to different applications");
        result.metadata = result.base ? selectedMetadata[0] : selectedMetadata[1];
        return result;
    }
}
