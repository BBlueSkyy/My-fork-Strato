// SPDX-License-Identifier: MPL-2.0
#include <cctype>
#include "program_content.h"

namespace skyline::loader {
    ProgramNcaSelection SelectProgramNcas(std::vector<ProgramNcaCandidate> candidates, const std::vector<vfs::CNMT> &metadata) {
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
                    if (record.contentType != vfs::ContentType::Program || record.idOffset != 0)
                        continue;
                    std::string filename;
                    for (const auto byte : record.contentId)
                        filename += fmt::format("{:02x}", byte);
                    filename += ".nca";
                    if (filename != candidate.filename)
                        continue;
                    const bool patch{meta.header.contentMetaType == vfs::ContentMetaType::Patch};
                    const u64 applicationId{patch ? meta.GetParentProgramId() : meta.header.id};
                    if (candidate.nca.header.titleId != applicationId)
                        throw exception("CNMT Program record disagrees with NCA Program ID");
                    auto &selected{patch ? result.patch : result.base};
                    if (selected && selected->header.titleId != candidate.nca.header.titleId)
                        throw exception("Container contains multiple initial applications");
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
            if (!matched && !hasProgramRecords) {
                auto &selected{candidate.nca.HasBktrSection() ? result.patch : result.base};
                if (selected)
                    throw exception("Ambiguous Program NCAs without CNMT records");
                selected = std::move(candidate.nca);
            }
        }
        if (result.base && result.patch && result.base->header.titleId != result.patch->header.titleId)
            throw exception("Program patch and base belong to different applications");
        result.metadata = result.base ? selectedMetadata[0] : selectedMetadata[1];
        return result;
    }
}
