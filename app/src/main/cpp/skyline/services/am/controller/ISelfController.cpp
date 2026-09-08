// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <nce.h>
#include <kernel/types/KProcess.h>
#include <services/hosbinder/IHOSBinderDriver.h>
#include "ISelfController.h"

namespace skyline::service::am {
    ISelfController::ISelfController(const DeviceState &state, ServiceManager &manager,
                                     std::shared_ptr<AppletState> appletState)
        : BaseService(state, manager), appletState(std::move(appletState)),
          hosbinder(manager.CreateOrGetService<hosbinder::IHOSBinderDriver>("dispdrv")) {}

    Result ISelfController::Exit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        throw nce::NCE::ExitException(true);
    }

    Result ISelfController::LockExit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->exitLocked = true;
        return {};
    }

    Result ISelfController::UnlockExit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->exitLocked = false;
        return {};
    }

    Result ISelfController::EnterFatalSection(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        ++appletState->fatalSectionCount;
        return {};
    }

    Result ISelfController::LeaveFatalSection(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        if (appletState->fatalSectionCount == 0)
            return result::FatalSectionCountImbalance;
        --appletState->fatalSectionCount;
        return {};
    }

    Result ISelfController::GetLibraryAppletLaunchableEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        appletState->libraryAppletLaunchableEvent->Signal();
        response.copyHandles.push_back(state.process->InsertItem(appletState->libraryAppletLaunchableEvent));
        return {};
    }

    Result ISelfController::SetScreenShotPermission(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->screenShotPermission = request.Pop<u32>();
        return {};
    }

    Result ISelfController::SetOperationModeChangedNotification(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->operationModeChangedNotification = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetPerformanceModeChangedNotification(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->performanceModeChangedNotification = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetFocusHandlingMode(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const bool notify{request.Pop<u8>() != 0};
        const bool background{request.Pop<u8>() != 0};
        const bool suspend{request.Pop<u8>() != 0};
        std::scoped_lock lock{appletState->mutex};
        appletState->focusStateChangedNotification = notify;
        appletState->focusBackgroundMode = background;
        appletState->focusSuspendingMode = suspend;
        return {};
    }

    Result ISelfController::SetRestartMessageEnabled(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->restartMessageEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetScreenShotAppletIdentityInfo(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const u32 appletId{request.Pop<u32>()};
        request.Skip<u32>();
        const u64 applicationId{request.Pop<u64>()};
        std::scoped_lock lock{appletState->mutex};
        appletState->screenShotAppletId = appletId;
        appletState->screenShotApplicationId = applicationId;
        return {};
    }

    Result ISelfController::SetOutOfFocusSuspendingEnabled(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->outOfFocusSuspendingEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetAlbumImageOrientation(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->screenShotImageOrientation = request.Pop<u32>();
        return {};
    }

    Result ISelfController::CreateManagedDisplayLayer(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        const auto layerId{hosbinder->CreateLayer(hosbinder::DisplayId::Default)};
        response.Push<u64>(layerId);
        return {};
    }

    Result ISelfController::CreateManagedDisplaySeparableLayer(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        // Eden also exposes a zero recording-layer id when the display backend only supports one layer.
        const auto layerId{hosbinder->CreateLayer(hosbinder::DisplayId::Default)};
        response.Push<u64>(layerId);
        response.Push<u64>(0);
        return {};
    }

    Result ISelfController::SetHandlesRequestToDisplay(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->handlesRequestToDisplay = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::ApproveToDisplay(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ISelfController::OverrideAutoSleepTimeAndDimmingTime(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        request.Pop<i32>();
        request.Pop<i32>();
        request.Pop<i32>();
        request.Pop<i32>();
        return {};
    }

    Result ISelfController::SetMediaPlaybackState(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->mediaPlaybackState = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetIdleTimeDetectionExtension(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->idleTimeDetectionExtension = request.Pop<u32>();
        return {};
    }

    Result ISelfController::GetIdleTimeDetectionExtension(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u32>(appletState->idleTimeDetectionExtension);
        return {};
    }

    Result ISelfController::ReportUserIsActive(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ISelfController::IsIlluminanceAvailable(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u8>(false);
        return {};
    }

    Result ISelfController::SetAutoSleepDisabled(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->autoSleepDisabled = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::IsAutoSleepDisabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u8>(appletState->autoSleepDisabled);
        return {};
    }

    Result ISelfController::GetCurrentIlluminanceEx(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(1);
        response.Push<float>(10000.0F);
        return {};
    }

    Result ISelfController::SetInputDetectionPolicy(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->inputDetectionPolicy = request.Pop<u32>();
        return {};
    }

    Result ISelfController::GetAccumulatedSuspendedTickValue(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u64>(appletState->accumulatedSuspendedTicks);
        return {};
    }

    Result ISelfController::GetAccumulatedSuspendedTickChangedEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(appletState->accumulatedSuspendedTickChangedEvent));
        return {};
    }

    Result ISelfController::SetAlbumImageTakenNotificationEnabled(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->albumImageTakenNotificationEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SaveCurrentScreenshot(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        [[maybe_unused]] const u32 reportOption{request.Pop<u32>()};
        // No capture backend is exposed here; Eden also succeeds when its screenshot service is unavailable.
        return {};
    }

    Result ISelfController::SetRecordVolumeMuted(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->recordVolumeMuted = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::Unknown230(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const u32 value{request.Pop<u32>()};
        response.Push<u16>(0);
        return {};
    }
}
