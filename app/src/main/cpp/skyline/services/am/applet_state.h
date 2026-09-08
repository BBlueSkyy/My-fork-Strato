// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <kernel/types/KEvent.h>

namespace skyline::service::am {
    class IStorage;

    /**
     * @brief State shared by the AM controller objects returned from one applet proxy.
     */
    struct AppletState {
        static constexpr u32 FocusStateChangedMessage{0xF};

        explicit AppletState(const DeviceState &state)
            : messageEvent(std::make_shared<type::KEvent>(state, false)),
              defaultDisplayResolutionChangeEvent(std::make_shared<type::KEvent>(state, false)),
              sleepLockEvent(std::make_shared<type::KEvent>(state, false)),
              hdcpStateChangeEvent(std::make_shared<type::KEvent>(state, false)),
              libraryAppletLaunchableEvent(std::make_shared<type::KEvent>(state, false)),
              accumulatedSuspendedTickChangedEvent(std::make_shared<type::KEvent>(state, true)),
              gpuErrorEvent(std::make_shared<type::KEvent>(state, false)),
              friendInvitationStorageChannelEvent(std::make_shared<type::KEvent>(state, false)),
              notificationStorageChannelEvent(std::make_shared<type::KEvent>(state, false)),
              healthWarningDisappearedEvent(std::make_shared<type::KEvent>(state, false)),
              unknownEvent210(std::make_shared<type::KEvent>(state, false)) {
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

        std::shared_ptr<type::KEvent> messageEvent;
        std::shared_ptr<type::KEvent> defaultDisplayResolutionChangeEvent;
        std::shared_ptr<type::KEvent> sleepLockEvent;
        std::shared_ptr<type::KEvent> hdcpStateChangeEvent;
        std::shared_ptr<type::KEvent> libraryAppletLaunchableEvent;
        std::shared_ptr<type::KEvent> accumulatedSuspendedTickChangedEvent;
        std::shared_ptr<type::KEvent> gpuErrorEvent;
        std::shared_ptr<type::KEvent> friendInvitationStorageChannelEvent;
        std::shared_ptr<type::KEvent> notificationStorageChannelEvent;
        std::shared_ptr<type::KEvent> healthWarningDisappearedEvent;
        std::shared_ptr<type::KEvent> unknownEvent210;

        std::deque<u32> messageQueue;
        std::deque<std::shared_ptr<IStorage>> userChannel;
        std::deque<std::shared_ptr<IStorage>> friendInvitationStorageChannel;
        std::deque<std::shared_ptr<IStorage>> notificationStorageChannel;
        std::deque<std::shared_ptr<IStorage>> processWindingContext;

        Result terminateResult{};
        i32 previousProgramIndex{-1};
        i32 fatalSectionCount{};

        bool exitLocked{};
        bool operationModeChangedNotification{};
        bool performanceModeChangedNotification{};
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
        bool albumImageTakenNotificationEnabled{};
        bool recordVolumeMuted{};
        bool foregroundRightsAcquired{};
        bool jitServiceLaunched{};
        bool saveDataSizeOverridden{};

        u8 screenShotPermission{};
        u8 focusState{1};
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
    };
}
