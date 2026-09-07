// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/strato-emu/)

#include <common.h>
#include <kernel/types/KProcess.h>
#include <vfs/region_backing.h>
#include "nca.h"
#include "xci.h"
#include "program_content.h"

namespace skyline::loader {
    XciLoader::XciLoader(const std::shared_ptr<vfs::Backing> &backing, const std::shared_ptr<crypto::KeyStore> &keyStore) {
        header = backing->Read<GamecardHeader>();

        if (header.magic != util::MakeMagic<u32>("HEAD"))
            throw exception("Invalid XCI file");

        xci = std::make_shared<vfs::PartitionFileSystem>(std::make_shared<vfs::RegionBacking>(backing, header.hfs0PartitionOffset, header.hfs0HeaderSize * sizeof(u32)));

        auto root{xci->OpenDirectory("", {false, true})};
        for (const auto &entry : root->Read()) {
            auto entryDir{std::make_shared<vfs::PartitionFileSystem>(xci->OpenFile(entry.name))};
            if (entry.name == "secure")
                secure = entryDir;
            else if (entry.name == "normal")
                normal = entryDir;
            else if (entry.name == "update")
                update = entryDir;
            else if (entry.name == "logo")
                logo = entryDir;
        }

        std::vector<ProgramNcaCandidate> programs;
        std::vector<vfs::CNMT> metadata;
        if (secure) {
            root = secure->OpenDirectory("", {false, true});
            for (const auto &entry : root->Read()) {
                if (entry.name.substr(entry.name.find_last_of('.') + 1) != "nca")
                    continue;

                try {
                    auto nca{vfs::NCA(secure->OpenFile(entry.name), keyStore, true)};

                    if (nca.contentType == vfs::NCAContentType::Program)
                        programs.push_back({entry.name, std::move(nca)});
                    else if (nca.contentType == vfs::NCAContentType::Control && nca.romFs != nullptr)
                        controlNca = std::move(nca);
                    else if (nca.contentType == vfs::NCAContentType::Meta) {
                        metadata.emplace_back(nca.cnmt);
                        metaNca = std::move(nca);
                    }
                } catch (const loader_exception &e) {
                    throw loader_exception(e.error);
                } catch (const std::exception &e) {
                    throw loader_exception(LoaderResult::ParsingError, fmt::format("NCA '{}': {}", entry.name, e.what()));
                }
            }
        } else {
            throw exception("Corrupted secure partition");
        }

        auto selection{SelectProgramNcas(std::move(programs), metadata)};
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
    }

    void *XciLoader::LoadProcessData(const std::shared_ptr<kernel::type::KProcess> &process, const DeviceState &state) {
        if (!programContentResolved || !processExeFs)
            throw exception("Program content must be resolved before loading the process");
        process->npdm = vfs::NPDM(processExeFs->OpenFile("main.npdm"));
        return NcaLoader::LoadExeFs(this, processExeFs, process, state);
    }

    std::vector<u8> XciLoader::GetIcon(language::ApplicationLanguage language) {
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
