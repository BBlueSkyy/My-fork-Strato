// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <atomic>
#include <thread>
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

    void OS::SetProgramLaunchContext(u8 programIndex, i32 previousIndex, std::vector<std::vector<u8>> userChannel) {
        std::scoped_lock lock{programExecutionMutex};
        currentProgramIndex = programIndex;
        previousProgramIndex = previousIndex;
        userChannelLaunchParameters = std::move(userChannel);
        programExecutionRequest.reset();
        programExecutionReady = false;
    }

    void OS::RequestProgramExecution(u8 programIndex, std::vector<std::vector<u8>> userChannel) {
        std::scoped_lock lock{programExecutionMutex};
        if (programExecutionRequest)
            throw exception("A program execution request is already pending");
        programExecutionRequest = ProgramExecutionRequest{programIndex, std::move(userChannel)};
    }

    void OS::NotifyProgramExecutionReady() {
        {
            std::scoped_lock lock{programExecutionMutex};
            if (!programExecutionRequest || programExecutionReady)
                return;
            programExecutionReady = true;
        }
        programExecutionCondition.notify_one();
    }

    bool OS::HasProgramExecutionRequest() {
        std::scoped_lock lock{programExecutionMutex};
        return programExecutionRequest.has_value();
    }

    std::optional<ProgramExecutionRequest> OS::TakeProgramExecutionRequest() {
        std::scoped_lock lock{programExecutionMutex};
        auto request{std::move(programExecutionRequest)};
        programExecutionRequest.reset();
        programExecutionReady = false;
        return request;
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

        LOGI("OS::Execute - romFd: {}, updateFd: {}, dlcFds count: {}, ProgramIndex: {}", romFd, updateFd, dlcFds.size(), currentProgramIndex);

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
            return;

        // ExecuteProgram is initiated from a guest IPC thread. Do not tear the process down from
        // that service handler. Wait until the IPC response has been written, then have a host-side
        // coordinator stop only HOS-1 so control returns to the frontend/JNI relaunch loop. The
        // normal process shutdown below remains responsible for stopping and joining every thread.
        std::atomic_bool executionFinished{};
        std::thread programHaltCoordinator{[this, process, &executionFinished] {
            u8 targetProgramIndex{};
            {
                std::unique_lock lock{programExecutionMutex};
                programExecutionCondition.wait(lock, [this, &executionFinished] {
                    return programExecutionReady || executionFinished.load(std::memory_order_acquire);
                });

                if (!programExecutionReady || !programExecutionRequest)
                    return;
                targetProgramIndex = programExecutionRequest->programIndex;
            }

            LOGI("ExecuteProgram: frontend halt requested for ProgramIndex {}", targetProgramIndex);
            process->Kill(false, false, true);
        }};

        auto finishProgramHaltCoordinator{[&] {
            executionFinished.store(true, std::memory_order_release);
            programExecutionCondition.notify_all();
            if (programHaltCoordinator.joinable())
                programHaltCoordinator.join();
        }};

        LOGI("Starting main HOS thread");
        try {
            thread->Start(true);
        } catch (...) {
            finishProgramHaltCoordinator();
            throw;
        }
        finishProgramHaltCoordinator();

        if (HasProgramExecutionRequest())
            LOGI("ExecuteProgram: main HOS returned to frontend");

        process->Kill(true, true, true);
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
