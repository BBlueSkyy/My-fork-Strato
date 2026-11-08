// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "gpu.h"
#include "audio.h"
#include "nce.h"
#include <jit/jit32.h>
#include "nce/guest.h"
#include "kernel/types/KProcess.h"
#include "vfs/os_backing.h"
#include "loader/nro.h"
#include "loader/nso.h"
#include "loader/nca.h"
#include "loader/nsp.h"
#include "loader/xci.h"
#include "services/account/IAccountServiceForApplication.h"
#include "services/fssrv/IFileSystemProxy.h"
#include "os.h"
#include <logger/logger.h>
#include <algorithm>
#include <cstdio>
#include <exception>
#include <new>
#include <limits.h>
#include <unistd.h>

namespace {
    std::optional<std::string> ResolveNroGuestPath(int fd, const std::string &publicAppFilesPath) {
        std::array<char, 64> fdPath{};
        std::snprintf(fdPath.data(), fdPath.size(), "/proc/self/fd/%d", fd);

        std::array<char, PATH_MAX + 1> hostPathBuffer{};
        const ssize_t length{readlink(fdPath.data(), hostPathBuffer.data(), PATH_MAX)};
        if (length <= 0)
            return std::nullopt;

        std::string hostPath{hostPathBuffer.data(), static_cast<size_t>(length)};
        std::string sdmcRoot{publicAppFilesPath};
        if (sdmcRoot.empty() || sdmcRoot.back() != '/')
            sdmcRoot.push_back('/');
        sdmcRoot += "switch/sdmc/";

        if (hostPath.size() <= sdmcRoot.size() || hostPath.compare(0, sdmcRoot.size(), sdmcRoot) != 0)
            return std::nullopt;

        return "sdmc:/" + hostPath.substr(sdmcRoot.size());
    }
}

namespace skyline::kernel {
    OS::OS(
        std::shared_ptr<JvmManager> &jvmManager,
        std::shared_ptr<Settings> &settings,
        std::string publicAppFilesPath,
        std::string privateAppFilesPath,
        std::string nativeLibraryPath,
        std::string deviceTimeZone,
        std::shared_ptr<vfs::FileSystem> assetFileSystem,
        std::shared_ptr<ProgramLaunchState> programLaunchState)
        : nativeLibraryPath(std::move(nativeLibraryPath)),
          publicAppFilesPath(std::move(publicAppFilesPath)),
          privateAppFilesPath(std::move(privateAppFilesPath)),
          deviceTimeZone(std::move(deviceTimeZone)),
          assetFileSystem(std::move(assetFileSystem)),
          programLaunchState(programLaunchState ? std::move(programLaunchState) : std::make_shared<ProgramLaunchState>()),
          state(this, jvmManager, settings),
          serviceManager(state) {}

    void OS::Execute(int romFd, std::vector<int> dlcFds, int updateFd, loader::RomFormat romType) {
        auto romFile{std::make_shared<vfs::OsBacking>(romFd)};
        keyStore = std::make_shared<crypto::KeyStore>(privateAppFilesPath + "keys/");

        LOGI("OS::Execute - romFd: {}, updateFd: {}, dlcFds count: {}", romFd, updateFd, dlcFds.size());

        const u8 programIndex{GetCurrentProgramIndex()};
        LOGI("Launching ProgramIndex {} (previous {})", programIndex, GetPreviousProgramIndex());
        state.loader = GetLoader(romFd, keyStore, romType, programIndex);

        if (updateFd >= 0) {
            LOGI("OS::Execute - Loading update from FD: {}", updateFd);
            // ManageContentActivity imports updates/DLC via NspFilePicker, even for an XCI base.
            state.updateLoader = GetLoader(updateFd, keyStore, loader::RomFormat::NSP, programIndex);
            LOGI("OS::Execute - Update loader created successfully");
        } else {
            LOGI("OS::Execute - No update to load (updateFd: {})", updateFd);
        }

        // DLC files are optional content. A malformed NSP must not prevent the
        // otherwise-valid base game/update from starting. Never publish a loader
        // until its metadata and referenced data storage have been validated.
        size_t rejectedDlcs{};
        for (size_t index{}; index < dlcFds.size(); ++index) {
            try {
                auto dlcLoader{GetLoader(dlcFds[index], keyStore, loader::RomFormat::NSP)};
                if (!dlcLoader || !dlcLoader->cnmt ||
                    dlcLoader->cnmt->GetContentMetaType() != vfs::ContentMetaType::AddOnContent) {
                    LOGW("Rejected DLC {}/{}: missing or non-AOC CNMT", index + 1, dlcFds.size());
                    ++rejectedDlcs;
                    continue;
                }

                const auto &contentInfos{dlcLoader->cnmt->GetContentInfos()};
                const bool expectsData{std::any_of(contentInfos.begin(), contentInfos.end(), [](const auto &info) {
                    return info.contentType == vfs::ContentType::Data;
                })};
                // Metadata-only entitlement DLCs are legitimate. A content record
                // claiming data, however, must not be advertised without a readable NCA.
                if ((expectsData && !dlcLoader->publicNca) ||
                    (dlcLoader->publicNca && !dlcLoader->publicNca->romFs)) {
                    LOGW("Rejected DLC {}/{} ({}): referenced data RomFS is unavailable",
                         index + 1, dlcFds.size(), dlcLoader->cnmt->GetTitleId());
                    ++rejectedDlcs;
                    continue;
                }

                LOGD("Accepted DLC {}/{}: {}", index + 1, dlcFds.size(), dlcLoader->cnmt->GetTitleId());
                state.dlcLoaders.push_back(std::move(dlcLoader));
            } catch (const std::bad_alloc &) {
                // Resource exhaustion is not malformed optional content.
                throw;
            } catch (const loader::loader_exception &e) {
                LOGW("Rejected DLC {}/{}: loader result {}: {}", index + 1, dlcFds.size(),
                     static_cast<int>(e.error), e.what());
                ++rejectedDlcs;
            } catch (const std::exception &e) {
                LOGW("Rejected DLC {}/{}: invalid or unreadable NSP: {}",
                     index + 1, dlcFds.size(), e.what());
                ++rejectedDlcs;
            }
        }
        if (rejectedDlcs)
            LOGW("DLC validation: {} accepted, {} rejected ({} selected); continuing startup",
                 state.dlcLoaders.size(), rejectedDlcs, dlcFds.size());
        else
            LOGD("DLC validation: {} accepted, 0 rejected", state.dlcLoaders.size());

        state.loader->ResolveProgramContent(state);

        if (state.loader->nacp) {
            const auto &nacp{state.loader->nacp->nacpContents};

            const auto cacheProvisionResult{service::fssrv::EnsureApplicationCacheStorage(publicAppFilesPath,
                                                                                           nacp.saveDataOwnerId,
                                                                                           nacp.cacheStorageSize,
                                                                                           nacp.cacheStorageJournalSize)};
            if (cacheProvisionResult)
                throw exception("Failed to provision application cache storage: {}", cacheProvisionResult.raw);

            const auto saveProvisionResult{service::fssrv::EnsureApplicationSaveData(publicAppFilesPath,
                                                                                     nacp.saveDataOwnerId,
                                                                                     constant::DefaultUserId,
                                                                                     nacp.userAccountSaveDataSize,
                                                                                     nacp.userAccountSaveDataJournalSize,
                                                                                     nacp.deviceSaveDataSize,
                                                                                     nacp.deviceSaveDataJournalSize)};
            if (saveProvisionResult)
                throw exception("Failed to provision application save data: {}", saveProvisionResult.raw);
        }

        state.gpu->Initialise();

        auto &process{state.process};
        process = std::make_shared<kernel::type::KProcess>(state);

        auto entry{state.loader->LoadProcessData(process, state)};
        state.audio->SetGuestAddressOffset(process->memory.TranslateVirtualAddress(0));
        auto &nacp{state.loader->nacp};
        if (nacp) {
            std::string name{nacp->GetApplicationName(language::ApplicationLanguage::AmericanEnglish)}, publisher{nacp->GetApplicationPublisher(language::ApplicationLanguage::AmericanEnglish)};
            if (name.empty())
                name = nacp->GetApplicationName(nacp->GetFirstSupportedTitleLanguage());
            if (publisher.empty())
                publisher = nacp->GetApplicationPublisher(nacp->GetFirstSupportedTitleLanguage());

            if (state.loader->programUpdateApplied && state.updateLoader && state.updateLoader->nacp)
                LOGINF("Applied update v{}", state.updateLoader->nacp->GetApplicationVersion());

            if (state.dlcLoaders.size() > 0)
                for (auto &loader : state.dlcLoaders)
                    if (loader->cnmt)
                        LOGD("Applied DLC {}", loader->cnmt->GetTitleId());

            LOGINF(R"(Starting "{}" ({}) v{} by "{}")", name, nacp->GetSaveDataOwnerId(),
                   state.loader->programUpdateApplied && state.updateLoader && state.updateLoader->nacp ? state.updateLoader->nacp->GetApplicationVersion() : nacp->GetApplicationVersion(), publisher);
        }

        // Scheduler retrieves information from the NPDM of the process so it needs to be initialized after the process is created
        state.scheduler = std::make_shared<kernel::Scheduler>(state);

        if (!process->is64bit()) { // 32-bit guests execute via Dynarmic instead of NCE.
            state.jit32 = std::make_shared<jit::Jit32>(state);
        }

        process->InitializeHeapTls();
        auto thread{process->CreateThread(entry, state.loader->GetMainThreadArgument())};
        if (thread) {
            state.loader->OnMainThreadCreated(process, thread->handle);
            LOGI("Starting main HOS thread");
            thread->Start(true);
            process->Kill(true, true, true);
            skyline::AsyncLogger::Finalize(true);
        }
    }

    std::shared_ptr<loader::Loader> OS::GetLoader(int fd, std::shared_ptr<crypto::KeyStore> keyStore, loader::RomFormat romType,
                                                  u8 programIndex) {
        auto file{std::make_shared<vfs::OsBacking>(fd)};
        switch (romType) {
            case loader::RomFormat::NRO: {
                auto launchPath{ResolveNroGuestPath(fd, publicAppFilesPath)};
                if (launchPath)
                    LOGI("NRO launch path: {}", *launchPath);
                else
                    LOGW("NRO is outside the emulated SD card; Homebrew ABI argv[0] will be omitted");
                return std::make_shared<loader::NroLoader>(std::move(file), std::move(launchPath));
            }
            case loader::RomFormat::NSO:
                return std::make_shared<loader::NsoLoader>(std::move(file));
            case loader::RomFormat::NCA:
                return std::make_shared<loader::NcaLoader>(std::move(file), std::move(keyStore));
            case loader::RomFormat::NSP:
                return std::make_shared<loader::NspLoader>(file, keyStore, std::string{}, loader::NspLoadMode::Full, programIndex);
            case loader::RomFormat::XCI:
                return std::make_shared<loader::XciLoader>(file, keyStore, programIndex);
            default:
                throw exception("Unsupported ROM extension.");
        }
    }

    u8 OS::GetCurrentProgramIndex() const {
        std::scoped_lock lock{programLaunchState->mutex};
        return programLaunchState->currentProgramIndex;
    }

    i32 OS::GetPreviousProgramIndex() const {
        std::scoped_lock lock{programLaunchState->mutex};
        return programLaunchState->previousProgramIndex;
    }

    void OS::ClearUserChannel() {
        std::scoped_lock lock{programLaunchState->mutex};
        programLaunchState->userChannel.clear();
    }

    void OS::PushUserChannel(std::vector<u8> data) {
        std::scoped_lock lock{programLaunchState->mutex};
        programLaunchState->userChannel.push_back(std::move(data));
    }

    std::optional<std::vector<u8>> OS::PopUserChannel() {
        std::scoped_lock lock{programLaunchState->mutex};
        if (programLaunchState->userChannel.empty())
            return std::nullopt;

        auto data{std::move(programLaunchState->userChannel.back())};
        programLaunchState->userChannel.pop_back();
        return data;
    }

    std::vector<std::vector<u8>> OS::GetUserChannelSnapshot() const {
        std::scoped_lock lock{programLaunchState->mutex};
        return programLaunchState->userChannel;
    }
}
