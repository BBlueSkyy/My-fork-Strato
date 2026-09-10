// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <array>
#include <mutex>
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

    /**
     * @brief Functions relating to an application's own current status.
     * @url https://switchbrew.org/wiki/Applet_Manager_services#ISelfController
     */
    class ISelfController : public BaseService {
      private:
        struct AppletIdentityInfo {
            u32 appletId;
            u32 padding;
            u64 applicationId;
        };
        static_assert(sizeof(AppletIdentityInfo) == 0x10);

        std::shared_ptr<kernel::type::KEvent> libraryAppletLaunchableEvent;
        std::shared_ptr<kernel::type::KEvent> accumulatedSuspendedTickChangedEvent;
        std::shared_ptr<hosbinder::IHOSBinderDriver> hosbinder;

        std::mutex fatalSectionMutex;
        u32 fatalSectionCount{};

        bool exitLocked{};
        i32 screenShotPermission{};
        bool operationModeChangedNotification{};
        bool performanceModeChangedNotification{};
        std::array<bool, 3> focusHandlingMode{};
        bool restartMessageEnabled{};
        AppletIdentityInfo screenShotAppletIdentityInfo{};
        bool outOfFocusSuspendingEnabled{};
        bool controllerFirmwareUpdateSection{};
        bool requiresCaptureButtonShortPressedMessage{};
        u32 albumImageOrientation{};
        u32 desirableKeyboardLayout{};

        u32 managedDisplayLayerSeparationMode{};
        bool recordingLayerCompositionEnabled{};
        bool handlesRequestToDisplay{};

        std::array<i32, 4> autoSleepTimeAndDimmingTime{};
        bool mediaPlaybackState{};
        u32 idleTimeDetectionExtension{};
        u32 inputDetectionSourceSet{};
        bool autoSleepDisabled{};
        u32 inputDetectionPolicy{};
        i32 wirelessPriorityMode{};

        bool albumImageTakenNotificationEnabled{};
        std::array<u8, 0x400> applicationAlbumUserData{};
        u32 applicationAlbumUserDataSize{};
        bool recordVolumeMuted{};

      public:
        ISelfController(const DeviceState &state, ServiceManager &manager);

        Result Exit(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result LockExit(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result UnlockExit(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result EnterFatalSection(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result LeaveFatalSection(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetLibraryAppletLaunchableEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetScreenShotPermission(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetOperationModeChangedNotification(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetPerformanceModeChangedNotification(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetFocusHandlingMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetRestartMessageEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetScreenShotAppletIdentityInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetOutOfFocusSuspendingEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetControllerFirmwareUpdateSection(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetRequiresCaptureButtonShortPressedMessage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetAlbumImageOrientation(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetDesirableKeyboardLayout(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result CreateManagedDisplayLayer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsSystemBufferSharingEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetSystemSharedLayerHandle(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetSystemSharedBufferHandle(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result CreateManagedDisplaySeparableLayer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetManagedDisplayLayerSeparationMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetRecordingLayerCompositionEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetHandlesRequestToDisplay(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result ApproveToDisplay(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result OverrideAutoSleepTimeAndDimmingTime(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetMediaPlaybackState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetIdleTimeDetectionExtension(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetIdleTimeDetectionExtension(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetInputDetectionSourceSet(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result ReportUserIsActive(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetCurrentIlluminance(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsIlluminanceAvailable(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetAutoSleepDisabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsAutoSleepDisabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result ReportMultimediaError(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetCurrentIlluminanceEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetInputDetectionPolicy(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetWirelessPriorityMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetAccumulatedSuspendedTickValue(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetAccumulatedSuspendedTickChangedEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetAlbumImageTakenNotificationEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetApplicationAlbumUserData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SaveCurrentScreenshot(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetRecordVolumeMuted(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetDebugStorageChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, ISelfController, Exit),
            SFUNC(0x1, ISelfController, LockExit),
            SFUNC(0x2, ISelfController, UnlockExit),
            SFUNC(0x3, ISelfController, EnterFatalSection),
            SFUNC(0x4, ISelfController, LeaveFatalSection),
            SFUNC(0x9, ISelfController, GetLibraryAppletLaunchableEvent),
            SFUNC(0xA, ISelfController, SetScreenShotPermission),
            SFUNC(0xB, ISelfController, SetOperationModeChangedNotification),
            SFUNC(0xC, ISelfController, SetPerformanceModeChangedNotification),
            SFUNC(0xD, ISelfController, SetFocusHandlingMode),
            SFUNC(0xE, ISelfController, SetRestartMessageEnabled),
            SFUNC(0xF, ISelfController, SetScreenShotAppletIdentityInfo),
            SFUNC(0x10, ISelfController, SetOutOfFocusSuspendingEnabled),
            SFUNC(0x11, ISelfController, SetControllerFirmwareUpdateSection),
            SFUNC(0x12, ISelfController, SetRequiresCaptureButtonShortPressedMessage),
            SFUNC(0x13, ISelfController, SetAlbumImageOrientation),
            SFUNC(0x14, ISelfController, SetDesirableKeyboardLayout),
            SFUNC(0x28, ISelfController, CreateManagedDisplayLayer),
            SFUNC(0x29, ISelfController, IsSystemBufferSharingEnabled),
            SFUNC(0x2A, ISelfController, GetSystemSharedLayerHandle),
            SFUNC(0x2B, ISelfController, GetSystemSharedBufferHandle),
            SFUNC(0x2C, ISelfController, CreateManagedDisplaySeparableLayer),
            SFUNC(0x2D, ISelfController, SetManagedDisplayLayerSeparationMode),
            SFUNC(0x2E, ISelfController, SetRecordingLayerCompositionEnabled),
            SFUNC(0x32, ISelfController, SetHandlesRequestToDisplay),
            SFUNC(0x33, ISelfController, ApproveToDisplay),
            SFUNC(0x3C, ISelfController, OverrideAutoSleepTimeAndDimmingTime),
            SFUNC(0x3D, ISelfController, SetMediaPlaybackState),
            SFUNC(0x3E, ISelfController, SetIdleTimeDetectionExtension),
            SFUNC(0x3F, ISelfController, GetIdleTimeDetectionExtension),
            SFUNC(0x40, ISelfController, SetInputDetectionSourceSet),
            SFUNC(0x41, ISelfController, ReportUserIsActive),
            SFUNC(0x42, ISelfController, GetCurrentIlluminance),
            SFUNC(0x43, ISelfController, IsIlluminanceAvailable),
            SFUNC(0x44, ISelfController, SetAutoSleepDisabled),
            SFUNC(0x45, ISelfController, IsAutoSleepDisabled),
            SFUNC(0x46, ISelfController, ReportMultimediaError),
            SFUNC(0x47, ISelfController, GetCurrentIlluminanceEx),
            SFUNC(0x48, ISelfController, SetInputDetectionPolicy),
            SFUNC(0x50, ISelfController, SetWirelessPriorityMode),
            SFUNC(0x5A, ISelfController, GetAccumulatedSuspendedTickValue),
            SFUNC(0x5B, ISelfController, GetAccumulatedSuspendedTickChangedEvent),
            SFUNC(0x64, ISelfController, SetAlbumImageTakenNotificationEnabled),
            SFUNC(0x6E, ISelfController, SetApplicationAlbumUserData),
            SFUNC(0x78, ISelfController, SaveCurrentScreenshot),
            SFUNC(0x82, ISelfController, SetRecordVolumeMuted),
            SFUNC(0x3E8, ISelfController, GetDebugStorageChannel)
        )
    };
}
