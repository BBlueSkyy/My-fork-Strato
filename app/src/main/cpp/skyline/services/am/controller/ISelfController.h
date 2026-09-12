// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/am/applet_state.h>
#include <services/serviceman.h>

namespace skyline::service::hosbinder {
    class IHOSBinderDriver;
}

namespace skyline::service::am {
    namespace self_controller_result {
        constexpr Result NotAvailable(128, 2);
        constexpr Result InvalidParameters(128, 506);
        constexpr Result FatalSectionCountImbalance(128, 512);
        constexpr Result ControllerFirmwareUpdateSectionAlreadySet(128, 513);
    }

    class ISelfController : public BaseService {
      private:
        std::shared_ptr<AppletState> appletState;
        std::shared_ptr<hosbinder::IHOSBinderDriver> hosbinder;

      public:
        ISelfController(const DeviceState &state, ServiceManager &manager, std::shared_ptr<AppletState> appletState);

        Result Exit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result LockExit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result UnlockExit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result EnterFatalSection(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result LeaveFatalSection(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetLibraryAppletLaunchableEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetScreenShotPermission(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetOperationModeChangedNotification(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetPerformanceModeChangedNotification(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetFocusHandlingMode(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetRestartMessageEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetScreenShotAppletIdentityInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetOutOfFocusSuspendingEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetControllerFirmwareUpdateSection(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetRequiresCaptureButtonShortPressedMessage(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetAlbumImageOrientation(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetDesirableKeyboardLayout(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result CreateManagedDisplayLayer(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result IsSystemBufferSharingEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetSystemSharedLayerHandle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetSystemSharedBufferHandle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result CreateManagedDisplaySeparableLayer(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetManagedDisplayLayerSeparationMode(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetRecordingLayerCompositionEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetHandlesRequestToDisplay(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ApproveToDisplay(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result OverrideAutoSleepTimeAndDimmingTime(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetMediaPlaybackState(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetIdleTimeDetectionExtension(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetIdleTimeDetectionExtension(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetInputDetectionSourceSet(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ReportUserIsActive(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetCurrentIlluminance(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result IsIlluminanceAvailable(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetAutoSleepDisabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result IsAutoSleepDisabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ReportMultimediaError(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetCurrentIlluminanceEx(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetInputDetectionPolicy(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetWirelessPriorityMode(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetAccumulatedSuspendedTickValue(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetAccumulatedSuspendedTickChangedEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetAlbumImageTakenNotificationEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetApplicationAlbumUserData(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SaveCurrentScreenshot(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetRecordVolumeMuted(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result Unknown230(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetDebugStorageChannel(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

        SERVICE_DECL(
            SFUNC(0, ISelfController, Exit),
            SFUNC(1, ISelfController, LockExit),
            SFUNC(2, ISelfController, UnlockExit),
            SFUNC(3, ISelfController, EnterFatalSection),
            SFUNC(4, ISelfController, LeaveFatalSection),
            SFUNC(9, ISelfController, GetLibraryAppletLaunchableEvent),
            SFUNC(10, ISelfController, SetScreenShotPermission),
            SFUNC(11, ISelfController, SetOperationModeChangedNotification),
            SFUNC(12, ISelfController, SetPerformanceModeChangedNotification),
            SFUNC(13, ISelfController, SetFocusHandlingMode),
            SFUNC(14, ISelfController, SetRestartMessageEnabled),
            SFUNC(15, ISelfController, SetScreenShotAppletIdentityInfo),
            SFUNC(16, ISelfController, SetOutOfFocusSuspendingEnabled),
            SFUNC(17, ISelfController, SetControllerFirmwareUpdateSection),
            SFUNC(18, ISelfController, SetRequiresCaptureButtonShortPressedMessage),
            SFUNC(19, ISelfController, SetAlbumImageOrientation),
            SFUNC(20, ISelfController, SetDesirableKeyboardLayout),
            SFUNC(40, ISelfController, CreateManagedDisplayLayer),
            SFUNC(41, ISelfController, IsSystemBufferSharingEnabled),
            SFUNC(42, ISelfController, GetSystemSharedLayerHandle),
            SFUNC(43, ISelfController, GetSystemSharedBufferHandle),
            SFUNC(44, ISelfController, CreateManagedDisplaySeparableLayer),
            SFUNC(45, ISelfController, SetManagedDisplayLayerSeparationMode),
            SFUNC(46, ISelfController, SetRecordingLayerCompositionEnabled),
            SFUNC(50, ISelfController, SetHandlesRequestToDisplay),
            SFUNC(51, ISelfController, ApproveToDisplay),
            SFUNC(60, ISelfController, OverrideAutoSleepTimeAndDimmingTime),
            SFUNC(61, ISelfController, SetMediaPlaybackState),
            SFUNC(62, ISelfController, SetIdleTimeDetectionExtension),
            SFUNC(63, ISelfController, GetIdleTimeDetectionExtension),
            SFUNC(64, ISelfController, SetInputDetectionSourceSet),
            SFUNC(65, ISelfController, ReportUserIsActive),
            SFUNC(66, ISelfController, GetCurrentIlluminance),
            SFUNC(67, ISelfController, IsIlluminanceAvailable),
            SFUNC(68, ISelfController, SetAutoSleepDisabled),
            SFUNC(69, ISelfController, IsAutoSleepDisabled),
            SFUNC(70, ISelfController, ReportMultimediaError),
            SFUNC(71, ISelfController, GetCurrentIlluminanceEx),
            SFUNC(72, ISelfController, SetInputDetectionPolicy),
            SFUNC(80, ISelfController, SetWirelessPriorityMode),
            SFUNC(90, ISelfController, GetAccumulatedSuspendedTickValue),
            SFUNC(91, ISelfController, GetAccumulatedSuspendedTickChangedEvent),
            SFUNC(100, ISelfController, SetAlbumImageTakenNotificationEnabled),
            SFUNC(110, ISelfController, SetApplicationAlbumUserData),
            SFUNC(120, ISelfController, SaveCurrentScreenshot),
            SFUNC(130, ISelfController, SetRecordVolumeMuted),
            SFUNC(230, ISelfController, Unknown230),
            SFUNC(1000, ISelfController, GetDebugStorageChannel)
        )
    };
}
