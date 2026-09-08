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
        static constexpr i64 DefaultSaveDataSize{200000000};
        static constexpr i64 DefaultJournalSaveDataSize{200000000};
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
        Result NotifyRunning(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetPseudoDeviceId(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetMediaPlaybackStateForApplication(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result IsGamePlayRecordingSupported(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result InitializeGamePlayRecording(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetGamePlayRecordingState(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result EnableApplicationCrashReport(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result InitializeApplicationCopyrightFrameBuffer(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetApplicationCopyrightImage(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetApplicationCopyrightVisibility(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result QueryApplicationPlayStatistics(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result QueryApplicationPlayStatisticsByUid(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ClearUserChannel(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result UnpopToUserChannel(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetPreviousProgramIndex(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetGpuErrorDetectedSystemEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetFriendInvitationStorageChannelEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result TryPopFromFriendInvitationStorageChannel(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetNotificationStorageChannelEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetHealthWarningDisappearedSystemEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetUnknownEvent210(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result Unknown330(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PrepareForJit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

        SERVICE_DECL(
            SFUNC(1, IApplicationFunctions, PopLaunchParameter),
            SFUNC(20, IApplicationFunctions, EnsureSaveData),
            SFUNC(21, IApplicationFunctions, GetDesiredLanguage),
            SFUNC(22, IApplicationFunctions, SetTerminateResult),
            SFUNC(23, IApplicationFunctions, GetDisplayVersion),
            SFUNC(25, IApplicationFunctions, ExtendSaveData),
            SFUNC(26, IApplicationFunctions, GetSaveDataSize),
            SFUNC(27, IApplicationFunctions, CreateCacheStorage),
            SFUNC(28, IApplicationFunctions, GetSaveDataSizeMax),
            SFUNC(29, IApplicationFunctions, GetCacheStorageMax),
            SFUNC(30, IApplicationFunctions, BeginBlockingHomeButtonShortAndLongPressed),
            SFUNC(31, IApplicationFunctions, EndBlockingHomeButtonShortAndLongPressed),
            SFUNC(32, IApplicationFunctions, BeginBlockingHomeButton),
            SFUNC(33, IApplicationFunctions, EndBlockingHomeButton),
            SFUNC(40, IApplicationFunctions, NotifyRunning),
            SFUNC(50, IApplicationFunctions, GetPseudoDeviceId),
            SFUNC(60, IApplicationFunctions, SetMediaPlaybackStateForApplication),
            SFUNC(65, IApplicationFunctions, IsGamePlayRecordingSupported),
            SFUNC(66, IApplicationFunctions, InitializeGamePlayRecording),
            SFUNC(67, IApplicationFunctions, SetGamePlayRecordingState),
            SFUNC(90, IApplicationFunctions, EnableApplicationCrashReport),
            SFUNC(100, IApplicationFunctions, InitializeApplicationCopyrightFrameBuffer),
            SFUNC(101, IApplicationFunctions, SetApplicationCopyrightImage),
            SFUNC(102, IApplicationFunctions, SetApplicationCopyrightVisibility),
            SFUNC(110, IApplicationFunctions, QueryApplicationPlayStatistics),
            SFUNC(111, IApplicationFunctions, QueryApplicationPlayStatisticsByUid),
            SFUNC(121, IApplicationFunctions, ClearUserChannel),
            SFUNC(122, IApplicationFunctions, UnpopToUserChannel),
            SFUNC(123, IApplicationFunctions, GetPreviousProgramIndex),
            SFUNC(130, IApplicationFunctions, GetGpuErrorDetectedSystemEvent),
            SFUNC(140, IApplicationFunctions, GetFriendInvitationStorageChannelEvent),
            SFUNC(141, IApplicationFunctions, TryPopFromFriendInvitationStorageChannel),
            SFUNC(150, IApplicationFunctions, GetNotificationStorageChannelEvent),
            SFUNC(160, IApplicationFunctions, GetHealthWarningDisappearedSystemEvent),
            SFUNC(210, IApplicationFunctions, GetUnknownEvent210),
            SFUNC(330, IApplicationFunctions, Unknown330),
            SFUNC(1001, IApplicationFunctions, PrepareForJit)
        )
    };
}
