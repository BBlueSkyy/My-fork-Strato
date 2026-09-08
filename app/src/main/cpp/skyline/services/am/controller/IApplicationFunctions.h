// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include <services/am/applet_state.h>

namespace skyline::service::am {
    namespace result {
        constexpr Result NotAvailable(128, 2);
        constexpr Result InvalidInput(128, 500);
        constexpr Result InvalidParameters(128, 506);
    }

    class IApplicationFunctions : public BaseService {
      private:
        static constexpr i64 SaveDataSize{200000000};
        static constexpr i64 JournalSaveDataSize{200000000};
        std::shared_ptr<AppletState> appletState;

      public:
        IApplicationFunctions(const DeviceState &state, ServiceManager &manager, std::shared_ptr<AppletState> appletState);

        Result PopLaunchParameter(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result EnsureSaveData(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetDesiredLanguage(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetTerminateResult(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetDisplayVersion(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ExtendSaveData(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetSaveDataSize(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result CreateCacheStorage(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetSaveDataSizeMax(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetCacheStorageMax(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result BeginBlockingHomeButtonShortAndLongPressed(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result EndBlockingHomeButtonShortAndLongPressed(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result BeginBlockingHomeButton(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result EndBlockingHomeButton(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetDeviceSaveDataSizeMax(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetLimitedApplicationLicenseUpgradableEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result NotifyRunning(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetPseudoDeviceId(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetMediaPlaybackStateForApplication(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result IsGamePlayRecordingSupported(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result InitializeGamePlayRecording(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetGamePlayRecordingState(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result RequestFlushGamePlayingMovieForDebug(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result EnableApplicationCrashReport(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result InitializeApplicationCopyrightFrameBuffer(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetApplicationCopyrightImage(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetApplicationCopyrightVisibility(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result QueryApplicationPlayStatistics(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result QueryApplicationPlayStatisticsByUid(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ClearUserChannel(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result UnpopToUserChannel(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetPreviousProgramIndex(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result EnableApplicationAllThreadDumpOnCrash(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetGpuErrorDetectedSystemEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetDelayTimeToAbortOnGpuError(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetFriendInvitationStorageChannelEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result TryPopFromFriendInvitationStorageChannel(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetNotificationStorageChannelEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result TryPopFromNotificationStorageChannel(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetHealthWarningDisappearedSystemEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetHdcpAuthenticationActivated(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetLaunchRequiredVersion(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result UpgradeLaunchRequiredVersion(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SendServerMaintenanceOverlayNotification(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetLastApplicationExitReason(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result Cmd210(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetAudioOutputPolicy(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result IsLanguageSelectionLimited(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

        SERVICE_DECL(
            SFUNC(0x1, IApplicationFunctions, PopLaunchParameter),
            SFUNC(0x14, IApplicationFunctions, EnsureSaveData),
            SFUNC(0x15, IApplicationFunctions, GetDesiredLanguage),
            SFUNC(0x16, IApplicationFunctions, SetTerminateResult),
            SFUNC(0x17, IApplicationFunctions, GetDisplayVersion),
            SFUNC(0x19, IApplicationFunctions, ExtendSaveData),
            SFUNC(0x1A, IApplicationFunctions, GetSaveDataSize),
            SFUNC(0x1B, IApplicationFunctions, CreateCacheStorage),
            SFUNC(0x1C, IApplicationFunctions, GetSaveDataSizeMax),
            SFUNC(0x1D, IApplicationFunctions, GetCacheStorageMax),
            SFUNC(0x1E, IApplicationFunctions, BeginBlockingHomeButtonShortAndLongPressed),
            SFUNC(0x1F, IApplicationFunctions, EndBlockingHomeButtonShortAndLongPressed),
            SFUNC(0x20, IApplicationFunctions, BeginBlockingHomeButton),
            SFUNC(0x21, IApplicationFunctions, EndBlockingHomeButton),
            SFUNC(0x23, IApplicationFunctions, GetDeviceSaveDataSizeMax),
            SFUNC(0x25, IApplicationFunctions, GetLimitedApplicationLicenseUpgradableEvent),
            SFUNC(0x28, IApplicationFunctions, NotifyRunning),
            SFUNC(0x32, IApplicationFunctions, GetPseudoDeviceId),
            SFUNC(0x3C, IApplicationFunctions, SetMediaPlaybackStateForApplication),
            SFUNC(0x41, IApplicationFunctions, IsGamePlayRecordingSupported),
            SFUNC(0x42, IApplicationFunctions, InitializeGamePlayRecording),
            SFUNC(0x43, IApplicationFunctions, SetGamePlayRecordingState),
            SFUNC(0x44, IApplicationFunctions, RequestFlushGamePlayingMovieForDebug),
            SFUNC(0x5A, IApplicationFunctions, EnableApplicationCrashReport),
            SFUNC(0x64, IApplicationFunctions, InitializeApplicationCopyrightFrameBuffer),
            SFUNC(0x65, IApplicationFunctions, SetApplicationCopyrightImage),
            SFUNC(0x66, IApplicationFunctions, SetApplicationCopyrightVisibility),
            SFUNC(0x6E, IApplicationFunctions, QueryApplicationPlayStatistics),
            SFUNC(0x6F, IApplicationFunctions, QueryApplicationPlayStatisticsByUid),
            SFUNC(0x79, IApplicationFunctions, ClearUserChannel),
            SFUNC(0x7A, IApplicationFunctions, UnpopToUserChannel),
            SFUNC(0x7B, IApplicationFunctions, GetPreviousProgramIndex),
            SFUNC(0x7C, IApplicationFunctions, EnableApplicationAllThreadDumpOnCrash),
            SFUNC(0x82, IApplicationFunctions, GetGpuErrorDetectedSystemEvent),
            SFUNC(0x83, IApplicationFunctions, SetDelayTimeToAbortOnGpuError),
            SFUNC(0x8C, IApplicationFunctions, GetFriendInvitationStorageChannelEvent),
            SFUNC(0x8D, IApplicationFunctions, TryPopFromFriendInvitationStorageChannel),
            SFUNC(0x96, IApplicationFunctions, GetNotificationStorageChannelEvent),
            SFUNC(0x97, IApplicationFunctions, TryPopFromNotificationStorageChannel),
            SFUNC(0xA0, IApplicationFunctions, GetHealthWarningDisappearedSystemEvent),
            SFUNC(0xAA, IApplicationFunctions, SetHdcpAuthenticationActivated),
            SFUNC(0xB4, IApplicationFunctions, GetLaunchRequiredVersion),
            SFUNC(0xB5, IApplicationFunctions, UpgradeLaunchRequiredVersion),
            SFUNC(0xBE, IApplicationFunctions, SendServerMaintenanceOverlayNotification),
            SFUNC(0xC8, IApplicationFunctions, GetLastApplicationExitReason),
            SFUNC(0xD2, IApplicationFunctions, Cmd210),
            SFUNC(0xDC, IApplicationFunctions, SetAudioOutputPolicy),
            SFUNC(0x14A, IApplicationFunctions, IsLanguageSelectionLimited)
        )
    };
}
