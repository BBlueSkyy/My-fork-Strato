// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

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
            storage = std::move(appletState->userChannel.front());
            appletState->userChannel.pop_front();
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
        // Save directories are materialized lazily by fsp-srv. Zero means no additional
        // allocation is required from the caller.
        response.Push<u64>(0);
        return {};
    }

    Result IApplicationFunctions::GetDesiredLanguage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto desiredLanguage{language::GetApplicationLanguage(*state.settings->systemLanguage)};
        if (((1U << static_cast<u32>(desiredLanguage)) & state.loader->nacp->nacpContents.supportedLanguageFlag) == 0)
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
        const auto saveDataType{request.Pop<u64>()};
        const auto userId{request.Pop<account::UserId>()};
        const auto normalSize{request.Pop<u64>()};
        const auto journalSize{request.Pop<u64>()};
        LOGD("ExtendSaveData type={} uid={:016X}{:016X} normal=0x{:X} journal=0x{:X}",
             saveDataType, userId.upper, userId.lower, normalSize, journalSize);
        response.Push<u64>(0);
        return {};
    }

    Result IApplicationFunctions::GetSaveDataSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto saveDataType{request.Pop<u64>()};
        [[maybe_unused]] const auto userId{request.Pop<account::UserId>()};

        // SaveDataType::Account is 1 and SaveDataType::Device is 3 on Horizon.
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
        const auto saveSize{request.Pop<i64>()};
        const auto journalSize{request.Pop<i64>()};
        if (saveSize < 0 || journalSize < 0)
            return result::InvalidParameters;

        LOGD("CreateCacheStorage index={} size=0x{:X} journal=0x{:X}", index, saveSize, journalSize);
        // u32 media + four bytes CMIF padding + u64 required size.
        response.Push<u64>(1); // Nand
        response.Push<u64>(0); // success: no additional space needs to be freed
        return {};
    }

    Result IApplicationFunctions::GetSaveDataSizeMax(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<i64>(SaveDataSize);
        response.Push<i64>(JournalSaveDataSize);
        return {};
    }

    Result IApplicationFunctions::GetCacheStorageMax(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // The simplified NACP parser does not expose the newer cache-max fields yet. Keep a
        // coherent single cache slot and a generous journal ceiling instead of returning no data.
        response.Push<i32>(0);
        response.Push<u32>(0); // CMIF padding
        response.Push<i64>(JournalSaveDataSize);
        return {};
    }

    Result IApplicationFunctions::BeginBlockingHomeButtonShortAndLongPressed(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto unused{request.Pop<i64>()};
        std::scoped_lock lock{appletState->mutex};
        appletState->homeButtonShortAndLongBlocked = true;
        return {};
    }

    Result IApplicationFunctions::EndBlockingHomeButtonShortAndLongPressed(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->homeButtonShortAndLongBlocked = false;
        return {};
    }

    Result IApplicationFunctions::BeginBlockingHomeButton(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto timeoutNs{request.Pop<i64>()};
        std::scoped_lock lock{appletState->mutex};
        appletState->homeButtonBlocked = true;
        return {};
    }

    Result IApplicationFunctions::EndBlockingHomeButton(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->homeButtonBlocked = false;
        return {};
    }

    Result IApplicationFunctions::GetDeviceSaveDataSizeMax(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<i64>(std::max<i64>(SaveDataSize, state.loader->nacp->nacpContents.deviceSaveDataSize));
        response.Push<i64>(std::max<i64>(JournalSaveDataSize, state.loader->nacp->nacpContents.deviceSaveDataJournalSize));
        return {};
    }

    Result IApplicationFunctions::GetLimitedApplicationLicenseUpgradableEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(appletState->limitedApplicationLicenseUpgradableEvent)};
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IApplicationFunctions::NotifyRunning(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(1);
        return {};
    }

    Result IApplicationFunctions::GetPseudoDeviceId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto seed{state.loader->nacp->nacpContents.seedForPseudoDeviceId};
        std::array<u8, 20> hashBuf{};
        if (int err{mbedtls_sha1_ret(seed.data(), seed.size(), hashBuf.data())}; err < 0)
            throw exception("Failed to hash device ID, err: {}", err);
        response.Push<UUID>(UUID::GenerateUuidV5(hashBuf));
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
        constexpr u64 RequiredSize{0x6000000};
        const u64 size{request.Pop<u64>()};
        if (size != RequiredSize || request.copyHandles.empty())
            return result::InvalidParameters;
        std::scoped_lock lock{appletState->mutex};
        appletState->gameplayRecordingInitialized = true;
        return {};
    }

    Result IApplicationFunctions::SetGamePlayRecordingState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const u32 recordingState{request.Pop<u32>()};
        if (recordingState > 1)
            return result::InvalidParameters;
        std::scoped_lock lock{appletState->mutex};
        appletState->gameplayRecordingState = recordingState;
        return {};
    }

    Result IApplicationFunctions::RequestFlushGamePlayingMovieForDebug(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IApplicationFunctions::EnableApplicationCrashReport(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->crashReportEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result IApplicationFunctions::InitializeApplicationCopyrightFrameBuffer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const i32 width{request.Pop<i32>()};
        const i32 height{request.Pop<i32>()};
        const u64 transferMemorySize{request.Pop<u64>()};
        constexpr i32 MaximumFbWidth{1280};
        constexpr i32 MaximumFbHeight{720};
        constexpr u64 RequiredFbAlignment{0x40000};
        if (width < 1 || height < 1 || width > MaximumFbWidth || height > MaximumFbHeight || !util::IsAligned(transferMemorySize, RequiredFbAlignment))
            return result::InvalidParameters;
        return {};
    }

    Result IApplicationFunctions::SetApplicationCopyrightImage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const i32 x{request.Pop<i32>()};
        const i32 y{request.Pop<i32>()};
        const i32 width{request.Pop<i32>()};
        const i32 height{request.Pop<i32>()};
        [[maybe_unused]] const auto originMode{request.Pop<i32>()};
        if (x < 0 || y < 0 || width < 1 || height < 1)
            return result::InvalidParameters;
        return {};
    }

    Result IApplicationFunctions::SetApplicationCopyrightVisibility(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto visibility{request.Pop<u8>()};
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

    Result IApplicationFunctions::ClearUserChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->userChannel.clear();
        return {};
    }

    Result IApplicationFunctions::UnpopToUserChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto storage{request.PopService<IStorage>(0, session)};
        std::scoped_lock lock{appletState->mutex};
        appletState->userChannel.emplace_front(std::move(storage));
        return {};
    }

    Result IApplicationFunctions::GetPreviousProgramIndex(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<i32>(appletState->previousProgramIndex);
        return {};
    }

    Result IApplicationFunctions::EnableApplicationAllThreadDumpOnCrash(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto enabled{request.Pop<u8>()};
        return {};
    }

    Result IApplicationFunctions::GetGpuErrorDetectedSystemEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(appletState->gpuErrorEvent)};
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IApplicationFunctions::SetDelayTimeToAbortOnGpuError(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const i64 delay{request.Pop<i64>()};
        if (delay < 0)
            return result::InvalidParameters;
        std::scoped_lock lock{appletState->mutex};
        appletState->gpuAbortDelayNs = static_cast<u64>(delay);
        return {};
    }

    Result IApplicationFunctions::GetFriendInvitationStorageChannelEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(appletState->friendInvitationStorageChannelEvent)};
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IApplicationFunctions::TryPopFromFriendInvitationStorageChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::shared_ptr<IStorage> storage;
        {
            std::scoped_lock lock{appletState->mutex};
            if (appletState->friendInvitationStorageChannel.empty())
                return result::NotAvailable;
            storage = std::move(appletState->friendInvitationStorageChannel.front());
            appletState->friendInvitationStorageChannel.pop_front();
            if (appletState->friendInvitationStorageChannel.empty())
                appletState->friendInvitationStorageChannelEvent->ResetSignal();
        }
        manager.RegisterService(storage, session, response);
        return {};
    }

    Result IApplicationFunctions::GetNotificationStorageChannelEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(appletState->notificationStorageChannelEvent)};
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IApplicationFunctions::TryPopFromNotificationStorageChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::shared_ptr<IStorage> storage;
        {
            std::scoped_lock lock{appletState->mutex};
            if (appletState->notificationStorageChannel.empty())
                return result::NotAvailable;
            storage = std::move(appletState->notificationStorageChannel.front());
            appletState->notificationStorageChannel.pop_front();
            if (appletState->notificationStorageChannel.empty())
                appletState->notificationStorageChannelEvent->ResetSignal();
        }
        manager.RegisterService(storage, session, response);
        return {};
    }

    Result IApplicationFunctions::GetHealthWarningDisappearedSystemEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(appletState->healthWarningDisappearedEvent)};
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IApplicationFunctions::SetHdcpAuthenticationActivated(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto activated{request.Pop<u8>()};
        appletState->hdcpStateChangeEvent->Signal();
        return {};
    }

    Result IApplicationFunctions::GetLaunchRequiredVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto applicationId{request.Pop<u64>()};
        [[maybe_unused]] const auto reserved{request.Pop<u64>()};
        std::array<u8, 0x40> launchRequiredVersion{};
        response.Push(launchRequiredVersion);
        return {};
    }

    Result IApplicationFunctions::UpgradeLaunchRequiredVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto version{request.Pop<std::array<u8, 0x40>>()};
        [[maybe_unused]] const auto applicationId{request.Pop<u64>()};
        [[maybe_unused]] const auto reserved{request.Pop<u64>()};
        return {};
    }

    Result IApplicationFunctions::SendServerMaintenanceOverlayNotification(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto start{request.Pop<i64>()};
        [[maybe_unused]] const auto end{request.Pop<i64>()};
        return {};
    }

    Result IApplicationFunctions::GetLastApplicationExitReason(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<i32>(appletState->lastApplicationExitReason);
        return {};
    }

    Result IApplicationFunctions::Cmd210(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(appletState->unknownEvent210)};
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IApplicationFunctions::SetAudioOutputPolicy(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const u8 policy{request.Pop<u8>()};
        return policy <= 1 ? Result{} : result::InvalidParameters;
    }

    Result IApplicationFunctions::IsLanguageSelectionLimited(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(0);
        return {};
    }
}
