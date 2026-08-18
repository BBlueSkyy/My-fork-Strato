// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/uuid.h>
#include <mbedtls/sha1.h>
#include <loader/loader.h>
#include <common/settings.h>
#include <kernel/types/KProcess.h>
#include <services/account/IAccountServiceForApplication.h>
#include <services/am/storage/VectorIStorage.h>
#include "IApplicationFunctions.h"

namespace skyline::service::am {
    IApplicationFunctions::IApplicationFunctions(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager),
          gpuErrorEvent(std::make_shared<type::KEvent>(state, false)),
          friendInvitationStorageChannelEvent(std::make_shared<type::KEvent>(state, false)),
          notificationStorageChannelEvent(std::make_shared<type::KEvent>(state, false)),
          unknownEvent210(std::make_shared<type::KEvent>(state, false)) {}

    Result IApplicationFunctions::PopLaunchParameter(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        constexpr u32 LaunchParameterMagic{0xC79497CA}; //!< The magic of the application launch parameters
        constexpr size_t LaunchParameterSize{0x88}; //!< The size of the launch parameter IStorage

        enum class LaunchParameterKind : u32 {
            UserChannel = 1,
            PreselectedUser = 2,
            Unknown = 3,
        } launchParameterKind{request.Pop<LaunchParameterKind>()};

        std::shared_ptr<IStorage> storageService;
        switch (launchParameterKind) {
            case LaunchParameterKind::UserChannel:
                return result::NotAvailable;

            case LaunchParameterKind::PreselectedUser: {
                storageService = std::make_shared<VectorIStorage>(state, manager, LaunchParameterSize);

                storageService->Push<u32>(LaunchParameterMagic);
                storageService->Push<u32>(1);
                storageService->Push(constant::DefaultUserId);

                break;
            }

            case LaunchParameterKind::Unknown:
                throw exception("Popping 'Unknown' Launch Parameter: {}", static_cast<u32>(launchParameterKind));

            default:
                return result::InvalidInput;
        }

        manager.RegisterService(storageService, session, response);
        return {};
    }

    Result IApplicationFunctions::EnsureSaveData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(0);
        return {};
    }

    Result IApplicationFunctions::SetTerminateResult(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto result{request.Pop<Result>()};
        LOGI("App set termination result: {}", result.raw);
        return {};
    }

    Result IApplicationFunctions::GetDesiredLanguage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto desiredLanguage{language::GetApplicationLanguage(*state.settings->systemLanguage)};

        // In the future we might want to trigger an UI dialog if the user-selected language is not available, for now it will use the first one available
        if (((1 << static_cast<u32>(desiredLanguage)) & state.loader->nacp->nacpContents.supportedLanguageFlag) == 0)
            desiredLanguage = state.loader->nacp->GetFirstSupportedLanguage();

        response.Push(language::GetLanguageCode(language::GetSystemLanguage(desiredLanguage)));
        return {};
    }

    Result IApplicationFunctions::GetDisplayVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(state.loader->nacp->nacpContents.displayVersion);
        return {};
    }

    Result IApplicationFunctions::GetSaveDataSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto saveDataType{request.Pop<u64>()};
        auto userId{request.Pop<account::UserId>()};
        LOGD("Save data type: {}, UserId: {:016X}{:016X}", saveDataType, userId.upper, userId.lower);

        response.Push(SaveDataSize);
        response.Push(JournalSaveDataSize);
        return {};
    }

    Result IApplicationFunctions::CreateCacheStorage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // CreateCacheStorage(u16 index, s64 saveSize, s64 journalSize) -> (u64 storageTarget, u64 requiredSize)
        // Each scalar input/output is padded out to its own 8-byte slot on the wire
        auto index{static_cast<u16>(request.Pop<u64>())};
        auto saveSize{request.Pop<i64>()};
        auto journalSize{request.Pop<i64>()};

        LOGD("Cache storage index: {}, save size: 0x{:X}, journal size: 0x{:X}", index, saveSize, journalSize);

        // As with EnsureSaveData, we don't track real storage quotas: the actual directory is
        // created lazily by IFileSystemProxy::OpenSaveDataFileSystem when the cache storage is
        // opened. We just acknowledge the request and echo back the requested size.
        // storageTarget is nn::fs::CacheStorageTargetMedia, 1 = Nand, which is the only backing
        // we provide cache storage on; this is informational for the caller in most titles
        constexpr u64 CacheStorageTargetNand{1};
        response.Push<u64>(CacheStorageTargetNand);
        response.Push<u64>(static_cast<u64>(saveSize + journalSize));
        return {};
    }

    Result IApplicationFunctions::GetSaveDataSizeMax(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(SaveDataSize);
        response.Push(JournalSaveDataSize);
        return {};
    }

    Result IApplicationFunctions::NotifyRunning(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(1);
        return {};
    }

    Result IApplicationFunctions::GetPseudoDeviceId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto seedForPseudoDeviceId{state.loader->nacp->nacpContents.seedForPseudoDeviceId};
        std::array<u8, 20> hashBuf{};

        // On HOS the seed from control.ncap is hashed together with the device specific device ID seed
        // for us it's enough to just hash the seed from control.nacp as it provides the same guarantees
        if (int err{mbedtls_sha1_ret(seedForPseudoDeviceId.data(), seedForPseudoDeviceId.size(), hashBuf.data())}; err < 0)
            throw exception("Failed to hash device ID, err: {}", err);

        response.Push<UUID>(UUID::GenerateUuidV5(hashBuf));
        return {};
    }

    Result IApplicationFunctions::InitializeGamePlayRecording(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IApplicationFunctions::SetGamePlayRecordingState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IApplicationFunctions::EnableApplicationCrashReport(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IApplicationFunctions::InitializeApplicationCopyrightFrameBuffer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 width{request.Pop<i32>()};
        i32 height{request.Pop<i32>()};
        u64 transferMemorySize{request.Pop<u64>()};

        constexpr i32 MaximumFbWidth{1280};
        constexpr i32 MaximumFbHeight{720};
        constexpr u64 RequiredFbAlignment{0x40000};

        if (width > MaximumFbWidth || height > MaximumFbHeight || !util::IsAligned(transferMemorySize, RequiredFbAlignment))
            return result::InvalidParameters;

        LOGD("Dimensions: ({}, {}) Transfer Memory Size: {}", width, height, transferMemorySize);

        return {};
    }

    Result IApplicationFunctions::SetApplicationCopyrightImage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 x{request.Pop<i32>()};
        i32 y{request.Pop<i32>()};
        i32 width{request.Pop<i32>()};
        i32 height{request.Pop<i32>()};

        enum class WindowOriginMode : i32 {
            LowerLeft,
            UpperLeft
        } originMode = request.Pop<WindowOriginMode>();

        if (y < 0 || x < 0 || width < 1 || height < 1)
            return result::InvalidParameters;

        LOGD("Position: ({}, {}) Dimensions: ({}, {}) Origin mode: {}", x, y, width, height, static_cast<i32>(originMode));
        return {};
    }

    Result IApplicationFunctions::SetApplicationCopyrightVisibility(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        u8 visiblity{request.Pop<u8>()};
        LOGD("Visiblity: {}", visiblity);
        return {};
    }

    Result IApplicationFunctions::QueryApplicationPlayStatistics(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u32>(0);
        return {};
    }

    Result IApplicationFunctions::QueryApplicationPlayStatisticsByUid(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u32>(0);
        return {};
    }

    Result IApplicationFunctions::GetPreviousProgramIndex(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<i32>(previousProgramIndex);
        return {};
    }

    Result IApplicationFunctions::GetGpuErrorDetectedSystemEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(gpuErrorEvent)};
        LOGD("GPU Error Event Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IApplicationFunctions::GetFriendInvitationStorageChannelEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(friendInvitationStorageChannelEvent)};
        LOGD("Friend Invitiation Storage Channel Event Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IApplicationFunctions::TryPopFromFriendInvitationStorageChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return result::NotAvailable;
    }

    Result IApplicationFunctions::GetNotificationStorageChannelEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(notificationStorageChannelEvent)};
        LOGW("Notification Storage Channel Event Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IApplicationFunctions::Cmd210(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Undocumented cmd added in FW 20.0.0, no input/output bytes, just an output handle.
        // We stub it out the same way as the other unimplemented system events: hand back a
        // valid handle to an event that is never signalled, so games that query it don't crash
        // on the missing HIPC function while waiting on an event that (as far as we know) is
        // tied to some system-level condition we don't emulate.
        auto handle{state.process->InsertItem(unknownEvent210)};
        LOGW("Stubbed cmd 210 (unknown system event) Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }
}
