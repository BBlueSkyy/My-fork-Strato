// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <nce.h>
#include <kernel/types/KProcess.h>
#include <services/hosbinder/IHOSBinderDriver.h>
#include "ISelfController.h"

namespace skyline::service::am {
    namespace {
        constexpr float DefaultIlluminanceLux{10000.0F};
        constexpr size_t MultimediaTelemetryReportSize{0x138};
    }

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
            return self_controller_result::FatalSectionCountImbalance;
        --appletState->fatalSectionCount;
        return {};
    }

    Result ISelfController::GetLibraryAppletLaunchableEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        appletState->libraryAppletLaunchableEvent->Signal();
        response.copyHandles.push_back(state.process->InsertItem(appletState->libraryAppletLaunchableEvent));
        return {};
    }

    Result ISelfController::SetScreenShotPermission(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto permission{request.Pop<i32>()};
        if (permission < 0 || permission > 2)
            return self_controller_result::InvalidParameters;

        std::scoped_lock lock{appletState->mutex};
        appletState->screenShotPermission = static_cast<u32>(permission);
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

    Result ISelfController::SetControllerFirmwareUpdateSection(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const bool enabled{request.Pop<u8>() != 0};
        std::scoped_lock lock{appletState->mutex};
        if (appletState->controllerFirmwareUpdateSection == enabled)
            return self_controller_result::ControllerFirmwareUpdateSectionAlreadySet;

        appletState->controllerFirmwareUpdateSection = enabled;
        return {};
    }

    Result ISelfController::SetRequiresCaptureButtonShortPressedMessage(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->requiresCaptureButtonShortPressedMessage = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetAlbumImageOrientation(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto orientation{request.Pop<u32>()};
        if (orientation > 3)
            return self_controller_result::InvalidParameters;

        std::scoped_lock lock{appletState->mutex};
        appletState->screenShotImageOrientation = orientation;
        return {};
    }

    Result ISelfController::SetDesirableKeyboardLayout(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->desirableKeyboardLayout = request.Pop<u32>();
        return {};
    }

    Result ISelfController::CreateManagedDisplayLayer(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        const auto layerId{hosbinder->CreateLayer(hosbinder::DisplayId::Default)};
        response.Push<u64>(layerId);
        return {};
    }

    Result ISelfController::IsSystemBufferSharingEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return self_controller_result::NotAvailable;
    }

    Result ISelfController::GetSystemSharedLayerHandle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return self_controller_result::NotAvailable;
    }

    Result ISelfController::GetSystemSharedBufferHandle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return self_controller_result::NotAvailable;
    }

    Result ISelfController::CreateManagedDisplaySeparableLayer(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        // Eden also exposes a zero recording-layer id when the display backend only supports one layer.
        const auto layerId{hosbinder->CreateLayer(hosbinder::DisplayId::Default)};
        response.Push<u64>(layerId);
        response.Push<u64>(0);
        return {};
    }

    Result ISelfController::SetManagedDisplayLayerSeparationMode(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto mode{request.Pop<u32>()};
        if (mode > 1)
            return self_controller_result::InvalidParameters;

        std::scoped_lock lock{appletState->mutex};
        appletState->managedDisplayLayerSeparationMode = mode;
        return {};
    }

    Result ISelfController::SetRecordingLayerCompositionEnabled(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->recordingLayerCompositionEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetHandlesRequestToDisplay(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const bool handlesRequestToDisplay{request.Pop<u8>() != 0};
        {
            std::scoped_lock lock{appletState->mutex};
            appletState->handlesRequestToDisplay = handlesRequestToDisplay;
        }

        if (!handlesRequestToDisplay)
            return ApproveToDisplay(session, request, response);

        return {};
    }

    Result ISelfController::ApproveToDisplay(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ISelfController::OverrideAutoSleepTimeAndDimmingTime(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::array<i32, 4> values{};
        for (auto &value : values)
            value = request.Pop<i32>();

        std::scoped_lock lock{appletState->mutex};
        appletState->autoSleepTimeAndDimmingTime = values;
        return {};
    }

    Result ISelfController::SetMediaPlaybackState(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->mediaPlaybackState = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetIdleTimeDetectionExtension(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto extension{request.Pop<u32>()};
        if (extension > 2)
            return self_controller_result::InvalidParameters;

        std::scoped_lock lock{appletState->mutex};
        appletState->idleTimeDetectionExtension = extension;
        return {};
    }

    Result ISelfController::GetIdleTimeDetectionExtension(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u32>(appletState->idleTimeDetectionExtension);
        return {};
    }

    Result ISelfController::SetInputDetectionSourceSet(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->inputDetectionSourceSet = request.Pop<u32>();
        return {};
    }

    Result ISelfController::ReportUserIsActive(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ISelfController::GetCurrentIlluminance(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<float>(DefaultIlluminanceLux);
        return {};
    }

    Result ISelfController::IsIlluminanceAvailable(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u8>(true);
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

    Result ISelfController::ReportMultimediaError(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto resultCode{request.Pop<Result>()};
        if (request.inputBuf.empty() || request.inputBuf.at(0).size_bytes() < MultimediaTelemetryReportSize)
            return self_controller_result::InvalidParameters;

        LOGW("Guest reported multimedia error 0x{:X}", resultCode.raw);
        return {};
    }

    Result ISelfController::GetCurrentIlluminanceEx(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(1);
        response.Push<float>(DefaultIlluminanceLux);
        return {};
    }

    Result ISelfController::SetInputDetectionPolicy(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto policy{request.Pop<u32>()};
        if (policy > 1)
            return self_controller_result::InvalidParameters;

        std::scoped_lock lock{appletState->mutex};
        appletState->inputDetectionPolicy = policy;
        return {};
    }

    Result ISelfController::SetWirelessPriorityMode(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto mode{request.Pop<i32>()};
        if (mode < 0 || mode > 1)
            return self_controller_result::InvalidParameters;

        std::scoped_lock lock{appletState->mutex};
        appletState->wirelessPriorityMode = mode;
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

    Result ISelfController::SetApplicationAlbumUserData(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (request.inputBuf.empty())
            return self_controller_result::InvalidParameters;

        const auto input{request.inputBuf.at(0)};
        if (input.size_bytes() > appletState->applicationAlbumUserData.size())
            return self_controller_result::InvalidParameters;

        std::scoped_lock lock{appletState->mutex};
        appletState->applicationAlbumUserData.fill(0);
        std::copy(input.begin(), input.end(), appletState->applicationAlbumUserData.begin());
        appletState->applicationAlbumUserDataSize = static_cast<u32>(input.size_bytes());
        return {};
    }

    Result ISelfController::SaveCurrentScreenshot(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto reportOption{request.Pop<i32>()};
        if (reportOption < 0 || reportOption > 3)
            return self_controller_result::InvalidParameters;

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

    Result ISelfController::GetDebugStorageChannel(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return self_controller_result::NotAvailable;
    }
}
