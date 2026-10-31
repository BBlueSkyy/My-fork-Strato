// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <kernel/types/KProcess.h>
#include <vfs/npdm.h>
#include <vfs/nacp.h>
#include <vfs/os_filesystem.h>
#include <vfs/layered_romfs.h>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include "nso.h"
#include "nca.h"

namespace skyline::loader {
    namespace {
        /** Minimal NACP for extracted dumps without an optional control.nacp.
         * Fields that cannot be inferred (save sizes, policies) remain zero.
         */
        class ExtractedNacpBacking final : public vfs::Backing {
          private:
            vfs::NACP::NacpData data{};

            size_t ReadImpl(span<u8> output, size_t offset) override {
                if (offset >= size)
                    return 0;
                const size_t length{std::min(output.size(), size - offset)};
                std::memcpy(output.data(), reinterpret_cast<const u8 *>(&data) + offset, length);
                return length;
            }

          public:
            ExtractedNacpBacking(u64 programId, std::string_view title)
                : Backing({true, false, false}, sizeof(data)) {
                data.saveDataOwnerId = programId;
                data.supportedLanguageFlag = 1; // American English fallback
                auto &name{data.titleEntries[0].applicationName};
                const auto length{std::min(title.size(), name.size() - 1)};
                std::memcpy(name.data(), title.data(), length);
                constexpr std::string_view version{"0.0.0"};
                std::memcpy(data.displayVersion.data(), version.data(), version.size());
            }
        };
    }
    NcaLoader::NcaLoader(std::shared_ptr<vfs::Backing> backing, std::shared_ptr<crypto::KeyStore> keyStore) {
        programNca.emplace(std::move(backing), std::move(keyStore));
        if (programNca->exeFs == nullptr)
            throw exception("Only NCAs with an ExeFS can be loaded directly");
        romFs = programNca->romFs;
    }

    void *NcaLoader::LoadExeFs(Loader *loader, const std::shared_ptr<vfs::FileSystem> &exeFs, const std::shared_ptr<kernel::type::KProcess> &process, const DeviceState &state) {
        if (exeFs == nullptr)
            throw exception("Cannot load a null ExeFS");

        if (!exeFs->FileExists("rtld"))
            throw exception("Cannot load an ExeFS that doesn't contain rtld");

        auto nsoFile{exeFs->OpenFile("rtld")};

        state.process->memory.InitializeVmm(process->npdm.meta.flags.type, process->npdm.meta.flags.enableAliasRegionExtraSize);

        auto loadInfo{NsoLoader::LoadNso(loader, nsoFile, process, state, 0, "rtld.nso")};
        u64 offset{loadInfo.size};
        u8 *base{loadInfo.base};
        void *entry{loadInfo.entry};

        LOGI("Loaded 'rtld.nso' at {} (.text @ {})", fmt::ptr(base), entry);

        for (const auto &nso : {"main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7", "sdk"}) {
            if (exeFs->FileExists(nso))
                nsoFile = exeFs->OpenFile(nso);
            else
                continue;

            loadInfo = NsoLoader::LoadNso(loader, nsoFile, process, state, offset, nso + std::string(".nso"), true);
            LOGI("Loaded '{}.nso' at {} (.text @ {})", nso, fmt::ptr(base + offset), loadInfo.entry);
            offset += loadInfo.size;
        }

        state.process->memory.InitializeRegions(span<u8>{base, offset});

        return entry;
    }

    void *NcaLoader::LoadProcessData(const std::shared_ptr<kernel::type::KProcess> &process, const DeviceState &state) {
        if (!programContentResolved || !processExeFs)
            throw exception("Program content must be resolved before loading the process");
        process->npdm = vfs::NPDM(processExeFs->OpenFile("main.npdm"));
        return LoadExeFs(this, processExeFs, process, state);
    }

    ExtractedLoader::ExtractedLoader(const std::string &gameDirectory) {
        const std::filesystem::path root{gameDirectory};
        const auto exeFsDirectory{root / "exefs"};
        if (!std::filesystem::is_directory(exeFsDirectory))
            throw exception("Extracted game has no exefs directory");

        auto exeFs{std::make_shared<vfs::OsFileSystem>(exeFsDirectory.string())};
        if (!exeFs->FileExists("main.npdm") || !exeFs->FileExists("main") || !exeFs->FileExists("rtld"))
            throw exception("Extracted ExeFS must contain main.npdm, main and rtld");

        const vfs::NPDM npdm(exeFs->OpenFile("main.npdm"));
        const auto controlPath{root / "control.nacp"};
        if (std::filesystem::is_regular_file(controlPath)) {
            vfs::OsFileSystem controlFs(root.string());
            nacp.emplace(controlFs.OpenFile("control.nacp"));
        } else {
            nacp.emplace(std::make_shared<ExtractedNacpBacking>(
                npdm.aci0.programId, root.filename().string()));
            LOGW("Extracted title has no control.nacp; using minimal fallback metadata without save-size information");
        }

        processExeFs = std::move(exeFs);

        const auto romFsDirectory{root / "romfs"};
        if (std::filesystem::is_directory(romFsDirectory)) {
            // Build a virtual RomFS header backed by the original extracted
            // files. No repacking or copying the data partition is required.
            romFs = std::make_shared<vfs::LayeredRomFsBacking>(
                nullptr, std::vector<std::filesystem::path>{romFsDirectory});
        }
    }

    ExtractedLoader::ExtractedLoader(const std::shared_ptr<JavaExtractedSource> &source) {
        if (!source)
            throw exception("Null extracted-game Android document source");
        auto exefs{std::make_shared<ExtractedExeFs>(source)};
        if (!exefs->FileExists("main.npdm") || !exefs->FileExists("main") || !exefs->FileExists("rtld"))
            throw exception("SAF ExeFS is missing main.npdm, main or rtld");

        const vfs::NPDM npdm(exefs->OpenFile("main.npdm"));
        if (auto control{source->OpenFile("control.nacp")})
            nacp.emplace(control);
        else {
            nacp.emplace(std::make_shared<ExtractedNacpBacking>(
                npdm.aci0.programId, source->title));
            LOGW("SAF extracted title missing control.nacp; save-size metadata is unknown");
        }

        processExeFs = exefs;
        std::vector<vfs::LayeredRomFsBacking::ExtractedFile> romFiles;
        for (const auto &file : source->files) {
            if (file.path.starts_with("romfs/")) {
                auto backing{source->OpenFile(file.path)};
                romFiles.emplace_back(vfs::LayeredRomFsBacking::ExtractedFile{
                    file.path.substr(sizeof("romfs/") - 1), std::move(backing)});
            }
        }
        if (!romFiles.empty())
            romFs = std::make_shared<vfs::LayeredRomFsBacking>(romFiles);
    }

    void *ExtractedLoader::LoadProcessData(const std::shared_ptr<kernel::type::KProcess> &process,
                                           const DeviceState &state) {
        if (!programContentResolved || !processExeFs)
            throw exception("Extracted Program content must be resolved before launch");
        process->npdm = vfs::NPDM(processExeFs->OpenFile("main.npdm"));
        return NcaLoader::LoadExeFs(this, processExeFs, process, state);
    }
}
