// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <kernel/types/KEvent.h>

namespace skyline::service::am {
    class IStorage;
    class ILibraryAppletAccessor;

    struct AppletState {
        static constexpr u32 FocusStateChangedMessage{0xF};

        explicit AppletState(const DeviceState &state)
            : messageEvent(std::make_shared<kernel::type::KEvent>(state, false)),
              defaultDisplayResolutionChangeEvent(std::make_shared<kernel::type::KEvent>(state, false)),
              sleepLockEvent(std::make_shared<kernel::type::KEvent>(state, false)),
              hdcpStateChangeEvent(std::make_shared<kernel::type::KEvent>(state, false)),
              libraryAppletLaunchableEvent(std::make_shared<kernel::type::KEvent>(state, false)),
              accumulatedSuspendedTickChangedEvent(std::make_shared<kernel::type::KEvent>(state, true)),
              gpuErrorEvent(std::make_shared<kernel::type::KEvent>(state, false)),
              friendInvitationStorageChannelEvent(std::make_shared<kernel::type::KEvent>(state, false)),
              notificationStorageChannelEvent(std::make_shared<kernel::type::KEvent>(state, false)),
              healthWarningDisappearedEvent(std::make_shared<kernel::type::KEvent>(state, false)),
              unknownEvent210(std::make_shared<kernel::type::KEvent>(state, false)),
              generalChannelEvent(std::make_shared<kernel::type::KEvent>(state, false)),
              hdcpAuthenticationFailedEvent(std::make_shared<kernel::type::KEvent>(state, false)) {
            QueueMessage(FocusStateChangedMessage);
        }

        void QueueMessage(u32 message) {
            std::scoped_lock lock{mutex};
            messageQueue.emplace_back(message);
            messageEvent->Signal();
        }

        bool PopMessage(u32 &message) {
            std::scoped_lock lock{mutex};
            if (messageQueue.empty())
                return false;
            message = messageQueue.front();
            messageQueue.pop_front();
            if (messageQueue.empty())
                messageEvent->ResetSignal();
            return true;
        }

        std::mutex mutex;

        std::shared_ptr<kernel::type::KEvent> messageEvent;
        std::shared_ptr<kernel::type::KEvent> defaultDisplayResolutionChangeEvent;
        std::shared_ptr<kernel::type::KEvent> sleepLockEvent;
        std::shared_ptr<kernel::type::KEvent> hdcpStateChangeEvent;
        std::shared_ptr<kernel::type::KEvent> libraryAppletLaunchableEvent;
        std::shared_ptr<kernel::type::KEvent> accumulatedSuspendedTickChangedEvent;
        std::shared_ptr<kernel::type::KEvent> gpuErrorEvent;
        std::shared_ptr<kernel::type::KEvent> friendInvitationStorageChannelEvent;
        std::shared_ptr<kernel::type::KEvent> notificationStorageChannelEvent;
        std::shared_ptr<kernel::type::KEvent> healthWarningDisappearedEvent;
        std::shared_ptr<kernel::type::KEvent> unknownEvent210;
        std::shared_ptr<kernel::type::KEvent> generalChannelEvent;
        std::shared_ptr<kernel::type::KEvent> hdcpAuthenticationFailedEvent;

        std::deque<u32> messageQueue;
        std::deque<std::shared_ptr<IStorage>> userChannel;
        std::deque<std::shared_ptr<IStorage>> generalChannel;
        std::deque<std::shared_ptr<IStorage>> friendInvitationStorageChannel;
        std::deque<std::shared_ptr<IStorage>> notificationStorageChannel;
        std::deque<std::shared_ptr<IStorage>> processWindingContext;
        std::shared_ptr<ILibraryAppletAccessor> reservedLibraryApplet;
        std::shared_ptr<ILibraryAppletAccessor> callingLibraryApplet;

        Result terminateResult{};
        i32 previousProgramIndex{-1};
        i32 lastApplicationExitReason{};
        i32 fatalSectionCount{};

        bool exitLocked{};
        bool operationModeChangedNotification{};
        bool performanceModeChangedNotification{};
        bool focusStateChangedNotification{};
        bool focusBackgroundMode{};
        bool focusSuspendingMode{};
        bool restartMessageEnabled{};
        bool outOfFocusSuspendingEnabled{};
        bool handlesRequestToDisplay{};
        bool autoSleepDisabled{};
        bool vrModeEnabled{};
        bool vrMode3dEnabled{};
        bool sleepLockAcquired{};
        bool mediaPlaybackState{};
        bool gamePlayRecordingSupported{};
        bool applicationCrashReportEnabled{};
        bool homeButtonShortPressedBlocked{};
        bool homeButtonLongPressedBlocked{};
        bool homeButtonDoubleClickEnabled{};
        bool handlingCaptureButtonShortPressedMessageEnabled{};
        bool handlingCaptureButtonLongPressedMessageEnabled{};
        bool albumImageTakenNotificationEnabled{};
        bool recordVolumeMuted{};
        bool foregroundRightsAcquired{};
        bool rejectToChangeIntoBackground{};
        bool appletWindowVisible{true};
        bool jitServiceLaunched{};
        bool saveDataSizeOverridden{};
        bool requestExitToLibraryAppletAtExecuteNextProgramEnabled{};
        bool unwindAfterReserved{};
        bool homeMenuForegroundLocked{};
        bool shutdownRequested{};
        bool rebootRequested{};

        u8 focusState{1};
        u32 screenShotPermission{};
        u32 screenShotAppletId{};
        u64 screenShotApplicationId{};
        u64 appletResourceUserId{};
        u32 cpuBoostMode{};
        i32 cpuBoostRequestPriority{};
        u32 gamePlayRecordingState{};
        u32 idleTimeDetectionExtension{};
        u32 applicationCoreUsageMode{};
        u32 screenShotImageOrientation{};
        u32 inputDetectionPolicy{};

        float displayMagnificationX{};
        float displayMagnificationY{};
        float displayMagnificationWidth{1.0F};
        float displayMagnificationHeight{1.0F};

        u64 accumulatedSuspendedTicks{};
        i64 saveDataSize{};
        i64 saveDataJournalSize{};
        i64 gpuTimeSliceBoost{};
        i64 appletGpuTimeSlice{};
    };
}
