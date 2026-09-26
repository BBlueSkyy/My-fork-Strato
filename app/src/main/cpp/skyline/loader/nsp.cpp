// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <fstream>
#include <kernel/types/KProcess.h>
#include <vfs/ticket.h>
#include "nca.h"
#include "nsp.h"
#include "program_content.h"

namespace skyline::loader {
    static void ExtractTickets(const std::shared_ptr<vfs::PartitionFileSystem>& dir, const std::shared_ptr<crypto::KeyStore> &keyStore) {
        std::vector<vfs::Ticket> tickets;

        auto dirContent{dir->OpenDirectory("", {false, true})};
        for (const auto &entry : dirContent->Read()) {
            if (entry.name.substr(entry.name.find_last_of('.') + 1) == "tik")
                tickets.emplace_back(dir->OpenFile(entry.name));
        }

        for (auto ticket : tickets) {
            auto titleKey{span(ticket.titleKeyBlock).subspan(0, 16).as<crypto::KeyStore::Key128>()};
            keyStore->PopulateTitleKey(ticket.rightsId, titleKey);
        }
    }

    NspLoader::NspLoader(const std::shared_ptr<vfs::Backing> &backing, const std::shared_ptr<crypto::KeyStore> &keyStore,
                         const std::string &diagnosticsPath, NspLoadMode loadMode)
        : nsp(std::make_shared<vfs::PartitionFileSystem>(backing)) {
        LOGI("DLC-TRACE NspLoader constructed PFS backing_size=0x{:X}", backing ? backing->size : 0);
        ExtractTickets(nsp, keyStore);
        LOGI("DLC-TRACE NspLoader tickets extracted");

        const auto ncaParseMode{loadMode == NspLoadMode::MetadataOnly ? vfs::NCAParseMode::MetadataOnly : vfs::NCAParseMode::Full};
        std::vector<ProgramNcaCandidate> programs;
        std::vector<vfs::CNMT> metadata;

        auto root{nsp->OpenDirectory("", {false, true})};
        auto entries{root->Read()};
        LOGI("DLC-TRACE NspLoader root entries={}", entries.size());
        for (const auto &entry : entries) {
            if (entry.name.substr(entry.name.find_last_of('.') + 1) != "nca")
                continue;

            LOGI("DLC-TRACE NspLoader parsing NCA '{}'", entry.name);
            try {
                auto nca{vfs::NCA(nsp->OpenFile(entry.name), keyStore, false, ncaParseMode)};
                LOGI("DLC-TRACE NspLoader parsed NCA '{}' contentType={}", entry.name, static_cast<u32>(nca.contentType));

                if (nca.contentType == vfs::NCAContentType::Program)
                    programs.push_back({entry.name, std::move(nca)});
                else if (nca.contentType == vfs::NCAContentType::Control && nca.romFs != nullptr)
                    controlNca = std::move(nca);
                else if (nca.contentType == vfs::NCAContentType::Meta) {
                    metadata.emplace_back(nca.cnmt);
                    metaNca = std::move(nca);
                } else if (nca.contentType == vfs::NCAContentType::PublicData || nca.contentType == vfs::NCAContentType::Data)
                    publicNca = std::move(nca);
            } catch (const loader_exception &e) {
                if (!diagnosticsPath.empty()) {
                    std::ofstream diag(diagnosticsPath, std::ios::app);
                    if (diag)
                        diag << "NCA '" << entry.name << "' failed, LoaderResult=" << static_cast<int>(e.error) << ", " << e.what() << "\n";
                }
                if (loadMode == NspLoadMode::Full)
                    throw loader_exception(e.error, e.what());
                LOGW("Skipping NCA '{}' while reading NSP metadata: {}", entry.name, e.what());
            } catch (const std::exception &e) {
                LOGE("NCA parsing failed for '{}': {}", entry.name, e.what());
                if (loadMode == NspLoadMode::Full)
                    throw loader_exception(LoaderResult::ParsingError, fmt::format("NCA '{}': {}", entry.name, e.what()));
            }
        }

        LOGI("DLC-TRACE NspLoader selecting program NCAs programs={} metadata={}", programs.size(), metadata.size());
        auto selection{SelectProgramNcas(std::move(programs), metadata)};
        LOGI("DLC-TRACE NspLoader selection complete base={} patch={} metadata={}",
             selection.base.has_value(), selection.patch.has_value(), selection.metadata.has_value());
        programNca = std::move(selection.base);
        programPatchNca = std::move(selection.patch);
        if (programNca)
            romFs = programNca->romFs;

        if (controlNca) {
            controlRomFs = std::make_shared<vfs::RomFileSystem>(controlNca->romFs);
            nacp.emplace(controlRomFs->OpenFile("control.nacp"));
        }

        if (metaNca)
            cnmt = vfs::CNMT(metaNca->cnmt);
        if (selection.metadata)
            cnmt = std::move(selection.metadata);

        LOGI("DLC-TRACE NspLoader complete has_program={} has_patch={} has_control={} has_meta={} has_public={} has_cnmt={}",
             programNca.has_value(), programPatchNca.has_value(), controlNca.has_value(), metaNca.has_value(),
             publicNca.has_value(), cnmt.has_value());
    }

    void *NspLoader::LoadProcessData(const std::shared_ptr<kernel::type::KProcess> &process, const DeviceState &state) {
        if (!programContentResolved || !processExeFs)
            throw exception("Program content must be resolved before loading the process");
        process->npdm = vfs::NPDM(processExeFs->OpenFile("main.npdm"));
        return NcaLoader::LoadExeFs(this, processExeFs, process, state);
    }

    std::vector<u8> NspLoader::GetIcon(language::ApplicationLanguage language) {
        if (controlRomFs == nullptr)
            return std::vector<u8>();

        std::shared_ptr<vfs::Backing> icon{};

        auto iconName{fmt::format("icon_{}.dat", language::ToString(language))};
        if (!(icon = controlRomFs->OpenFileUnchecked(iconName, {true, false, false}))) {
            iconName = fmt::format("icon_{}.dat", language::ToString(nacp->GetFirstSupportedTitleLanguage()));
            icon = controlRomFs->OpenFileUnchecked(iconName, {true, false, false});
        }

        if (icon == nullptr)
            return std::vector<u8>();

        std::vector<u8> buffer(icon->size);
        icon->Read(buffer);
        return buffer;
    }
}
