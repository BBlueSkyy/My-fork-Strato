// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "gpu.h"
#include "nce.h"
#include "nce/guest.h"
#include "kernel/types/KProcess.h"
#include "vfs/os_backing.h"
#include "loader/nro.h"
#include "loader/nso.h"
#include "loader/nca.h"
#include "loader/nsp.h"
#include "loader/xci.h"
#include "os.h"
#include <logger/logger.h>

namespace skyline::kernel {
    OS::OS(
        std::shared_ptr<JvmManager> &jvmManager,
        std::shared_ptr<Settings> &settings,
        std::string publicAppFilesPath,
        std::string privateAppFilesPath,
        std::string nativeLibraryPath,
        std::string deviceTimeZone,
        std::shared_ptr<vfs::FileSystem> assetFileSystem)
        : nativeLibraryPath(std::move(nativeLibraryPath)),
          publicAppFilesPath(std::move(publicAppFilesPath)),
          privateAppFilesPath(std::move(privateAppFilesPath)),
          deviceTimeZone(std::move(deviceTimeZone)),
          assetFileSystem(std::move(assetFileSystem)),
          state(this, jvmManager, settings),
          serviceManager(state) {}

    void OS::Execute(int romFd, std::vector<int> dlcFds, int updateFd, loader::RomFormat romType, size_t programIndex, i32 previousProgramIndex) {
        auto romFile{std::make_shared<vfs::OsBacking>(romFd)};
        keyStore = std::make_shared<crypto::KeyStore>(privateAppFilesPath + "keys/");
        state.currentProgramIndex = static_cast<i32>(programIndex);
        state.previousProgramIndex = previousProgramIndex;

        LOGI("OS::Execute - romFd: {}, updateFd: {}, dlcFds count: {}", romFd, updateFd, dlcFds.size());

        state.loader = GetLoader(romFd, keyStore, romType, programIndex);

        if (updateFd > 0) {
            LOGI("OS::Execute - Loading update from FD: {}", updateFd);
            state.updateLoader = GetLoader(updateFd, keyStore, romType, programIndex);
            LOGI("OS::Execute - Update loader created successfully");
        } else {
            LOGI("OS::Execute - No update to load (updateFd: {})", updateFd);
        }

        if (dlcFds.size() > 0)
            for (int fd : dlcFds)
                state.dlcLoaders.push_back(GetLoader(fd, keyStore, romType, programIndex));

        state.gpu->Initialise();

        auto &process{state.process};
        process = std::make_shared<kernel::type::KProcess>(state);

        auto entry{state.loader->LoadProcessData(process, state)};
        auto &nacp{state.loader->nacp};
        if (nacp) {
            std::string name{nacp->GetApplicationName(language::ApplicationLanguage::AmericanEnglish)}, publisher{nacp->GetApplicationPublisher(language::ApplicationLanguage::AmericanEnglish)};
            if (name.empty())
                name = nacp->GetApplicationName(nacp->GetFirstSupportedTitleLanguage());
            if (publisher.empty())
                publisher = nacp->GetApplicationPublisher(nacp->GetFirstSupportedTitleLanguage());

            if (state.updateLoader)
                LOGINF("Applied update v{}", state.updateLoader->nacp->GetApplicationVersion());

            if (state.dlcLoaders.size() > 0)
                for (auto &loader : state.dlcLoaders)
                    LOGINF("Applied DLC {}", loader->cnmt->GetTitleId());

            LOGINF(R"(Starting "{}" ({}) v{} by "{}")", name, nacp->GetSaveDataOwnerId(), state.updateLoader ? state.updateLoader->nacp->GetApplicationVersion() : nacp->GetApplicationVersion(), publisher);
        }

        process->InitializeHeapTls();
        auto thread{process->CreateThread(entry)};
        if (thread) {
            LOGI("Starting main HOS thread");
            thread->Start(true);
            process->Kill(true, true, true);
        }
    }

    bool OS::RequestProgramChange(u64 programIndex) {
        const auto index{static_cast<size_t>(programIndex)};
        if (!state.loader || !state.loader->HasProgram(index))
            return false;

        requestedProgramIndex = index;
        return true;
    }

    std::optional<size_t> OS::TakeRequestedProgramIndex() {
        auto requested{requestedProgramIndex};
        requestedProgramIndex.reset();
        return requested;
    }

    std::shared_ptr<loader::Loader> OS::GetLoader(int fd, std::shared_ptr<crypto::KeyStore> keyStore, loader::RomFormat romType, size_t programIndex) {
        auto file{std::make_shared<vfs::OsBacking>(fd)};
        switch (romType) {
            case loader::RomFormat::NRO:
                return std::make_shared<loader::NroLoader>(std::move(file));
            case loader::RomFormat::NSO:
                return std::make_shared<loader::NsoLoader>(std::move(file));
            case loader::RomFormat::NCA:
                return std::make_shared<loader::NcaLoader>(std::move(file), std::move(keyStore));
            case loader::RomFormat::NSP:
                return std::make_shared<loader::NspLoader>(file, keyStore, "", loader::NspLoadMode::Full, programIndex);
            case loader::RomFormat::XCI:
                return std::make_shared<loader::XciLoader>(file, keyStore, programIndex);
            default:
                throw exception("Unsupported ROM extension.");
        }
    }
}
