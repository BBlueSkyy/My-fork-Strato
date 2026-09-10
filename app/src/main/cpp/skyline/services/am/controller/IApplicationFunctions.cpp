// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <array>
#include <common/uuid.h>
#include <mbedtls/sha1.h>
#include <loader/loader.h>
#include <common/settings.h>
#include <kernel/types/KProcess.h>
#include <services/account/IAccountServiceForApplication.h>
#include <services/am/storage/VectorIStorage.h>
#include "IApplicationFunctions.h"

namespace skyline::service::am {
    IApplicationFunctions::IApplicationFunctions(const DeviceState &state, ServiceManager &manager, std::shared_ptr<AppletState> appletState)
        : BaseService(state, manager), appletState(std::move(appletState)) {}

    Result IApplicationFunctions::PopLaunchParameter(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        constexpr u32 LaunchParameterMagic{0xC79497CA};
        constexpr size_t LaunchParameterSize{0x88};

        enum class LaunchParameterKind : u32 {
            UserChannel = 1,
            PreselectedUser = 2,
            Unknown = 3,
        };

        const auto kind{request.Pop<LaunchParameterKind>()};
        std::shared_ptr<IStorage> storage;

        if (kind == LaunchParameterKind::UserChannel) {
            std::scoped_lock lock{appletState->mutex};
            if (appletState->userChannel.empty())
                return result::NotAvailable;
            storage = std::move(appletState->userChannel.back());
            appletState->userChannel.pop_back();
        } else if (kind == LaunchParameterKind::PreselectedUser) {
            storage = std::make_shared<VectorIStorage>(state, manager, LaunchParameterSize);
            storage->Push<u32>(LaunchParameterMagic);
            storage->Push<u32>(1);
            storage->Push(constant::DefaultUserId);
        } else if (kind == LaunchParameterKind::Unknown) {
            return result::NotAvailable;
        } else {
            return result::InvalidInput;
        }

        manager.RegisterService(storage, session, response);
        return {};
    }

    Result IApplicationFunctions::EnsureSaveData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto userId{request.Pop<account::UserId>()};
        // fsp-srv creates the directory lazily in Strato; no extra allocation is required here.
        response.Push<u64>(0);
        return {};
    }

    Result IApplicationFunctions::GetDesiredLanguage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto desiredLanguage{language::GetApplicationLanguage(*state.settings->systemLanguage)};
        const u32 supported{state.loader->nacp->nacpContents.supportedLanguageFlag};
        if (supported != 0 && ((1U << static_cast<u32>(desiredLanguage)) & supported) == 0)
            desiredLanguage = state.loader->nacp->GetFirstSupportedLanguage();
        response.Push(language::GetLanguageCode(language::GetSystemLanguage(desiredLanguage)));
        return {};
    }

    Result IApplicationFunctions::SetTerminateResult(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto terminateResult{request.Pop<Result>()};
        {
            std::scoped_lock lock{appletState->mutex};
            appletState->terminateResult = terminateResult;
        }
        LOGI("App set termination result: {}", terminateResult.raw);
        return {};
    }

    Result IApplicationFunctions::GetDisplayVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(state.loader->nacp->nacpContents.displayVersion);
        return {};
    }

    Result IApplicationFunctions::ExtendSaveData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto saveDataType{request.Pop<u64>()};
        [[maybe_unused]] const auto userId{request.Pop<account::UserId>()};
        const i64 saveDataSize{request.Pop<i64>()};
        const i64 journalSize{request.Pop<i64>()};
        if (saveDataSize < 0 || journalSize < 0)
            return result::InvalidParameters;

        {
            std::scoped_lock lock{appletState->mutex};
            appletState->saveDataSize = saveDataSize;
            appletState->saveDataJournalSize = journalSize;
            appletState->saveDataSizeOverridden = true;
        }

        response.Push<u64>(0);
        return {};
    }

    Result IApplicationFunctions::GetSaveDataSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto saveDataType{request.Pop<u64>()};
        [[maybe_unused]] const auto userId{request.Pop<account::UserId>()};

        {
            std::scoped_lock lock{appletState->mutex};
            if (appletState->saveDataSizeOverridden) {
                response.Push<i64>(appletState->saveDataSize);
                response.Push<i64>(appletState->saveDataJournalSize);
                return {};
            }
        }

        if (saveDataType == 3) {
            response.Push<u64>(state.loader->nacp->nacpContents.deviceSaveDataSize);
            response.Push<u64>(state.loader->nacp->nacpContents.deviceSaveDataJournalSize);
        } else {
            response.Push<u64>(state.loader->nacp->nacpContents.userAccountSaveDataSize);
            response.Push<u64>(state.loader->nacp->nacpContents.userAccountSaveDataJournalSize);
        }
        return {};
    }

    Result IApplicationFunctions::CreateCacheStorage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto index{static_cast<u16>(request.Pop<u64>())};
        const i64 saveSize{request.Pop<i64>()};
        const i64 journalSize{request.Pop<i64>()};
        if (saveSize < 0 || journalSize < 0)
            return result::InvalidParameters;

        LOGD("CreateCacheStorage index={} size=0x{:X} journal=0x{:X}", index, saveSize, journalSize);
        response.Push<u32>(1); // CacheStorageTargetMedia::Nand
        response.Push<u32>(0); // CMIF alignment
        response.Push<u64>(0); // required size on success
        return {};
    }

    Result IApplicationFunctions::GetSaveDataSizeMax(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto &nacp{state.loader->nacp->nacpContents};
        const i64 normalMax{nacp.userAccountSaveDataSizeMax > 0 ? nacp.userAccountSaveDataSizeMax : DefaultSaveDataSize};
        const i64 journalMax{nacp.userAccountSaveDataJournalSizeMax > 0 ? nacp.userAccountSaveDataJournalSizeMax : DefaultJournalSaveDataSize};
        response.Push<i64>(normalMax);
        response.Push<i64>(journalMax);
        return {};
    }

    Result IApplicationFunctions::GetCacheStorageMax(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto &nacp{state.loader->nacp->nacpContents};
        response.Push<u32>(nacp.cacheStorageIndexMax);
        response.Push<u32>(0); // CMIF alignment
        response.Push<u64>(static_cast<u64>(std::max<i64>(nacp.cacheStorageDataAndJournalSizeMax, 0)));
        return {};
    }

    Result IApplicationFunctions::BeginBlockingHomeButtonShortAndLongPressed(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const i64 unused{request.Pop<i64>()};
        std::scoped_lock lock{appletState->mutex};
        appletState->homeButtonShortPressedBlocked = true;
        appletState->homeButtonLongPressedBlocked = true;
        return {};
    }

    Result IApplicationFunctions::EndBlockingHomeButtonShortAndLongPressed(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->homeButtonShortPressedBlocked = false;
        appletState->homeButtonLongPressedBlocked = false;
        return {};
    }

    Result IApplicationFunctions::BeginBlockingHomeButton(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const i64 timeoutNs{request.Pop<i64>()};
        std::scoped_lock lock{appletState->mutex};
        appletState->homeButtonShortPressedBlocked = true;
        appletState->homeButtonLongPressedBlocked = true;
        appletState->homeButtonDoubleClickEnabled = true;
        return {};
    }

    Result IApplicationFunctions::EndBlockingHomeButton(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->homeButtonShortPressedBlocked = false;
        appletState->homeButtonLongPressedBlocked = false;
        appletState->homeButtonDoubleClickEnabled = false;
        return {};
    }

    Result IApplicationFunctions::NotifyRunning(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(true);
        return {};
    }

    Result IApplicationFunctions::GetPseudoDeviceId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto seed{state.loader->nacp->nacpContents.seedForPseudoDeviceId};
        std::array<u8, 20> hash{};
        if (int err{mbedtls_sha1_ret(seed.data(), seed.size(), hash.data())}; err < 0)
            throw exception("Failed to hash pseudo device ID seed, err: {}", err);
        response.Push<UUID>(UUID::GenerateUuidV5(hash));
        return {};
    }

    Result IApplicationFunctions::SetMediaPlaybackStateForApplication(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->mediaPlaybackState = request.Pop<u8>() != 0;
        return {};
    }

    Result IApplicationFunctions::IsGamePlayRecordingSupported(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(state.loader->nacp->nacpContents.videoCaptureMode != 0);
        return {};
    }

    Result IApplicationFunctions::InitializeGamePlayRecording(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const u64 transferMemorySize{request.Pop<u64>()};
        // Eden and Ryujinx both accept this path without emulating the capture backend.
        return {};
    }

    Result IApplicationFunctions::SetGamePlayRecordingState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->gamePlayRecordingState = request.Pop<u32>();
        return {};
    }

    Result IApplicationFunctions::EnableApplicationCrashReport(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->applicationCrashReportEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result IApplicationFunctions::InitializeApplicationCopyrightFrameBuffer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const i32 width{request.Pop<i32>()};
        const i32 height{request.Pop<i32>()};
        const u64 transferMemorySize{request.Pop<u64>()};
        if (width < 1 || height < 1 || width > 1280 || height > 720 || !util::IsAligned(transferMemorySize, 0x40000UL))
            return result::InvalidParameters;
        return {};
    }

    Result IApplicationFunctions::SetApplicationCopyrightImage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const i32 x{request.Pop<i32>()};
        const i32 y{request.Pop<i32>()};
        const i32 width{request.Pop<i32>()};
        const i32 height{request.Pop<i32>()};
        [[maybe_unused]] const i32 originMode{request.Pop<i32>()};
        return (x < 0 || y < 0 || width < 1 || height < 1) ? result::InvalidParameters : Result{};
    }

    Result IApplicationFunctions::SetApplicationCopyrightVisibility(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const bool visible{request.Pop<u8>() != 0};
        return {};
    }

    Result IApplicationFunctions::QueryApplicationPlayStatistics(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<i32>(0);
        return {};
    }

    Result IApplicationFunctions::QueryApplicationPlayStatisticsByUid(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<i32>(0);
        return {};
    }

    Result IApplicationFunctions::ClearUserChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->userChannel.clear();
        return {};
    }

    Result IApplicationFunctions::UnpopToUserChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto storage{request.PopService<IStorage>(0, session)};
        std::scoped_lock lock{appletState->mutex};
        appletState->userChannel.emplace_back(std::move(storage));
        return {};
    }

    Result IApplicationFunctions::GetPreviousProgramIndex(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<i32>(appletState->previousProgramIndex);
        return {};
    }

    Result IApplicationFunctions::GetGpuErrorDetectedSystemEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(appletState->gpuErrorEvent));
        return {};
    }

    Result IApplicationFunctions::GetFriendInvitationStorageChannelEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(appletState->friendInvitationStorageChannelEvent));
        return {};
    }

    Result IApplicationFunctions::TryPopFromFriendInvitationStorageChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::shared_ptr<IStorage> storage;
        {
            std::scoped_lock lock{appletState->mutex};
            if (appletState->friendInvitationStorageChannel.empty())
                return result::NotAvailable;
            storage = std::move(appletState->friendInvitationStorageChannel.back());
            appletState->friendInvitationStorageChannel.pop_back();
        }
        manager.RegisterService(storage, session, response);
        return {};
    }

    Result IApplicationFunctions::GetNotificationStorageChannelEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(appletState->notificationStorageChannelEvent));
        return {};
    }

    Result IApplicationFunctions::GetHealthWarningDisappearedSystemEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(appletState->healthWarningDisappearedEvent));
        return {};
    }

    Result IApplicationFunctions::GetUnknownEvent210(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(appletState->unknownEvent210));
        return {};
    }

    Result IApplicationFunctions::Unknown330(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(0);
        return {};
    }

    Result IApplicationFunctions::PrepareForJit(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->jitServiceLaunched = true;
        return {};
    }
}
