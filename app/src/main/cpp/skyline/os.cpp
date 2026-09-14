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

    void OS::RequestProgramExecution(u8 programIndex, std::vector<std::vector<u8>> userChannel) {
        std::scoped_lock lock{programExecutionMutex};
        if (programExecutionRequest)
            throw exception("A program execution request is already pending");
        programExecutionRequest = ProgramExecutionRequest{programIndex, std::move(userChannel)};
    }

    bool OS::HasProgramExecutionRequest() {
        std::scoped_lock lock{programExecutionMutex};
        return programExecutionRequest.has_value();
    }

    u8 OS::GetCurrentProgramIndex() const {
        return currentProgramIndex;
    }

    i32 OS::GetPreviousProgramIndex() const {
        return previousProgramIndex;
    }

    std::vector<std::vector<u8>> OS::TakeUserChannelLaunchParameters() {
        std::scoped_lock lock{programExecutionMutex};
        std::vector<std::vector<u8>> parameters;
        parameters.swap(userChannelLaunchParameters);
        return parameters;
    }

    void OS::Execute(int romFd, std::vector<int> dlcFds, int updateFd, loader::RomFormat romType) {
        keyStore = std::make_shared<crypto::KeyStore>(privateAppFilesPath + "keys/");

        LOGI("OS::Execute - romFd: {}, updateFd: {}, dlcFds count: {}", romFd, updateFd, dlcFds.size());

        bool gpuInitialised{};
        while (true) {
            state.loader = GetLoader(romFd, keyStore, romType, currentProgramIndex);

            if (updateFd >= 0) {
                LOGI("OS::Execute - Loading update from FD: {}", updateFd);
                // ManageContentActivity imports updates/DLC via NspFilePicker, even for an XCI base.
                state.updateLoader = GetLoader(updateFd, keyStore, loader::RomFormat::NSP, currentProgramIndex);
                LOGI("OS::Execute - Update loader created successfully");
            } else {
                state.updateLoader.reset();
                LOGI("OS::Execute - No update to load (updateFd: {})", updateFd);
            }

            state.dlcLoaders.clear();
            for (int fd : dlcFds)
                state.dlcLoaders.push_back(GetLoader(fd, keyStore, loader::RomFormat::NSP));

            state.loader->ResolveProgramContent(state);

            if (!gpuInitialised) {
                state.gpu->Initialise();
                gpuInitialised = true;
            }

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

                if (state.loader->programUpdateApplied && state.updateLoader && state.updateLoader->nacp)
                    LOGINF("Applied update v{}", state.updateLoader->nacp->GetApplicationVersion());

                for (auto &loader : state.dlcLoaders)
                    if (loader->cnmt)
                        LOGINF("Applied DLC {}", loader->cnmt->GetTitleId());

                LOGINF(R"(Starting "{}" ({}) v{} by "{}" [ProgramIndex {}])", name, nacp->GetSaveDataOwnerId(),
                       state.loader->programUpdateApplied && state.updateLoader && state.updateLoader->nacp ? state.updateLoader->nacp->GetApplicationVersion() : nacp->GetApplicationVersion(),
                       publisher, currentProgramIndex);
            }

            process->InitializeHeapTls();
            auto thread{process->CreateThread(entry)};
            if (!thread)
                break;

            LOGI("Starting main HOS thread");
            thread->Start(true);
            process->Kill(true, true, true);

            std::optional<ProgramExecutionRequest> nextProgram;
            {
                std::scoped_lock lock{programExecutionMutex};
                nextProgram = std::move(programExecutionRequest);
                programExecutionRequest.reset();
            }

            if (!nextProgram)
                break;

            const auto oldProgramIndex{currentProgramIndex};
            previousProgramIndex = oldProgramIndex;
            currentProgramIndex = nextProgram->programIndex;
            {
                std::scoped_lock lock{programExecutionMutex};
                userChannelLaunchParameters = std::move(nextProgram->userChannel);
            }

            LOGI("ExecuteProgram: switching ProgramIndex {} -> {}", oldProgramIndex, currentProgramIndex);

            state.process.reset();
            state.loader.reset();
            state.updateLoader.reset();
            state.dlcLoaders.clear();
        }

        skyline::AsyncLogger::Finalize(true);
    }

    std::shared_ptr<loader::Loader> OS::GetLoader(int fd, std::shared_ptr<crypto::KeyStore> keyStore, loader::RomFormat romType, u8 programIndex) {
        auto file{std::make_shared<vfs::OsBacking>(fd)};
        switch (romType) {
            case loader::RomFormat::NRO:
                if (programIndex != 0)
                    throw exception("NRO does not support ProgramIndex switching");
                return std::make_shared<loader::NroLoader>(std::move(file));
            case loader::RomFormat::NSO:
                if (programIndex != 0)
                    throw exception("NSO does not support ProgramIndex switching");
                return std::make_shared<loader::NsoLoader>(std::move(file));
            case loader::RomFormat::NCA:
                if (programIndex != 0)
                    throw exception("Standalone NCA does not support ProgramIndex switching");
                return std::make_shared<loader::NcaLoader>(std::move(file), std::move(keyStore));
            case loader::RomFormat::NSP:
                return std::make_shared<loader::NspLoader>(file, keyStore, std::string{}, loader::NspLoadMode::Full, programIndex);
            case loader::RomFormat::XCI:
                return std::make_shared<loader::XciLoader>(file, keyStore, programIndex);
            default:
                throw exception("Unsupported ROM extension.");
        }
    }
}
