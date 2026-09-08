// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <kernel/types/KEvent.h>

namespace skyline::service::am {
    class IStorage;

    /**
     * @brief Process-wide AM state shared by all controller objects returned by a proxy.
     *
     * Horizon exposes the individual AM controller interfaces as views over one applet state.
     * Keeping that state here prevents each IPC object from inventing its own lifecycle/event
     * state and gives modern nnSdk versions coherent answers across interfaces.
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
              limitedApplicationLicenseUpgradableEvent(std::make_shared<type::KEvent>(state, false)),
              unknownEvent210(std::make_shared<type::KEvent>(state, false)) {
            themeStorage.fill(0xAA);
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
        std::shared_ptr<type::KEvent> limitedApplicationLicenseUpgradableEvent;
        std::shared_ptr<type::KEvent> unknownEvent210;

        std::deque<u32> messageQueue;
        std::deque<std::shared_ptr<IStorage>> userChannel;
        std::deque<std::shared_ptr<IStorage>> friendInvitationStorageChannel;
        std::deque<std::shared_ptr<IStorage>> notificationStorageChannel;
        std::deque<std::shared_ptr<IStorage>> appletBoundChannel;

        std::array<u8, 0x400> themeStorage{};

        Result terminateResult{};
        i32 previousProgramIndex{-1};
        i32 lastApplicationExitReason{};

        bool exitLocked{};
        bool operationModeChangedNotification{};
        bool performanceModeChangedNotification{};
        bool restartMessageEnabled{};
        bool outOfFocusSuspendingEnabled{};
        bool requestExitToLibraryAppletAtExecuteNextProgramEnabled{};
        bool handlesRequestToDisplay{};
        bool autoSleepDisabled{};
        bool vrModeEnabled{};
        bool vrMode3dEnabled{};
        bool sleepLockAcquired{};
        bool sleepDisabledTillShutdown{};
        bool sleepDisablingSuppressed{};
        bool mediaPlaybackState{};
        bool gameplayRecordingInitialized{};
        bool crashReportEnabled{};
        bool homeButtonShortAndLongBlocked{};
        bool homeButtonBlocked{};
        bool homeButtonDoubleClickEnabled{};
        bool handlingHomeButtonShortPressedEnabled{};
        bool albumImageTakenNotificationEnabled{};
        bool recordVolumeMuted{};
        bool foregroundRightsAcquired{};
        bool screenShotPermission{true};

        u32 focusState{1};
        u32 cpuBoostMode{};
        u32 cpuBoostRequestPriority{};
        u32 gameplayRecordingState{};
        u32 idleTimeDetectionExtension{};
        u32 applicationCoreUsageMode{};
        u32 screenShotImageOrientation{};
        u32 desirableKeyboardLayout{};
        u32 inputDetectionSourceSet{};
        u32 mediaPlaybackStateRaw{};
        u32 displayMagnificationX{};
        u32 displayMagnificationY{};
        u32 displayMagnificationWidth{};
        u32 displayMagnificationHeight{};

        u64 accumulatedSuspendedTicks{};
        u64 wakeupCount{};
        u64 gpuAbortDelayNs{};
        u64 launchRequiredVersion{};
    };
}
