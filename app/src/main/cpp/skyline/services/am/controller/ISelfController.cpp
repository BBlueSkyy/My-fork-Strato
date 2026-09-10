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

    ISelfController::ISelfController(const DeviceState &state, ServiceManager &manager)
        : libraryAppletLaunchableEvent(std::make_shared<type::KEvent>(state, false)),
          accumulatedSuspendedTickChangedEvent(std::make_shared<type::KEvent>(state, true)),
          hosbinder(manager.CreateOrGetService<hosbinder::IHOSBinderDriver>("dispdrv")),
          BaseService(state, manager) {}

    Result ISelfController::Exit(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        throw nce::NCE::ExitException(true);
    }

    Result ISelfController::LockExit(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        exitLocked = true;
        return {};
    }

    Result ISelfController::UnlockExit(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        exitLocked = false;
        return {};
    }

    Result ISelfController::EnterFatalSection(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{fatalSectionMutex};
        ++fatalSectionCount;
        return {};
    }

    Result ISelfController::LeaveFatalSection(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{fatalSectionMutex};
        if (!fatalSectionCount)
            return self_controller_result::FatalSectionCountImbalance;

        --fatalSectionCount;
        return {};
    }

    Result ISelfController::GetLibraryAppletLaunchableEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        libraryAppletLaunchableEvent->Signal();

        KHandle handle{state.process->InsertItem(libraryAppletLaunchableEvent)};
        LOGD("Library Applet Launchable Event Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }

    Result ISelfController::SetScreenShotPermission(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto permission{request.Pop<i32>()};
        if (permission < 0 || permission > 2)
            return self_controller_result::InvalidParameters;

        screenShotPermission = permission;
        return {};
    }

    Result ISelfController::SetOperationModeChangedNotification(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        operationModeChangedNotification = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetPerformanceModeChangedNotification(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        performanceModeChangedNotification = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetFocusHandlingMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        focusHandlingMode[0] = request.Pop<u8>() != 0;
        focusHandlingMode[1] = request.Pop<u8>() != 0;
        focusHandlingMode[2] = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetRestartMessageEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        restartMessageEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetScreenShotAppletIdentityInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        screenShotAppletIdentityInfo = request.Pop<AppletIdentityInfo>();
        return {};
    }

    Result ISelfController::SetOutOfFocusSuspendingEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        outOfFocusSuspendingEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetControllerFirmwareUpdateSection(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        bool enabled{request.Pop<u8>() != 0};
        if (controllerFirmwareUpdateSection == enabled)
            return self_controller_result::ControllerFirmwareUpdateSectionAlreadySet;

        controllerFirmwareUpdateSection = enabled;
        return {};
    }

    Result ISelfController::SetRequiresCaptureButtonShortPressedMessage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        requiresCaptureButtonShortPressedMessage = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetAlbumImageOrientation(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto orientation{request.Pop<u32>()};
        if (orientation > 3)
            return self_controller_result::InvalidParameters;

        albumImageOrientation = orientation;
        return {};
    }

    Result ISelfController::SetDesirableKeyboardLayout(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        desirableKeyboardLayout = request.Pop<u32>();
        return {};
    }

    Result ISelfController::CreateManagedDisplayLayer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto layerId{hosbinder->CreateLayer(hosbinder::DisplayId::Default)};
        LOGD("Creating Managed Layer #{} on 'Default' Display", layerId);
        response.Push(layerId);
        return {};
    }

    Result ISelfController::IsSystemBufferSharingEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // The current HOSBinder stack has no fbshare/shared-layer implementation.
        return self_controller_result::NotAvailable;
    }

    Result ISelfController::GetSystemSharedLayerHandle(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return self_controller_result::NotAvailable;
    }

    Result ISelfController::GetSystemSharedBufferHandle(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return self_controller_result::NotAvailable;
    }

    Result ISelfController::CreateManagedDisplaySeparableLayer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto layerId{hosbinder->CreateLayer(hosbinder::DisplayId::Default)};

        // HOS creates a second recording layer here. Strato's HOSBinder intentionally supports
        // one layer only, so use the same compatibility behavior as Eden until that changes.
        constexpr u64 RecordingLayerId{};
        response.Push(layerId);
        response.Push(RecordingLayerId);

        LOGD("Creating Managed Separable Layer #{} (recording layer unavailable)", layerId);
        return {};
    }

    Result ISelfController::SetManagedDisplayLayerSeparationMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto mode{request.Pop<u32>()};
        if (mode > 1)
            return self_controller_result::InvalidParameters;

        managedDisplayLayerSeparationMode = mode;
        return {};
    }

    Result ISelfController::SetRecordingLayerCompositionEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        recordingLayerCompositionEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetHandlesRequestToDisplay(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        handlesRequestToDisplay = request.Pop<u8>() != 0;
        if (!handlesRequestToDisplay)
            return ApproveToDisplay(session, request, response);

        return {};
    }

    Result ISelfController::ApproveToDisplay(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // RequestToDisplay messaging requires shared applet lifecycle state, which the current
        // AM proxy architecture does not expose to this service instance.
        return {};
    }

    Result ISelfController::OverrideAutoSleepTimeAndDimmingTime(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        for (auto &value : autoSleepTimeAndDimmingTime)
            value = request.Pop<i32>();

        return {};
    }

    Result ISelfController::SetMediaPlaybackState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        mediaPlaybackState = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetIdleTimeDetectionExtension(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto extension{request.Pop<u32>()};
        if (extension > 2)
            return self_controller_result::InvalidParameters;

        idleTimeDetectionExtension = extension;
        return {};
    }

    Result ISelfController::GetIdleTimeDetectionExtension(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(idleTimeDetectionExtension);
        return {};
    }

    Result ISelfController::SetInputDetectionSourceSet(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        inputDetectionSourceSet = request.Pop<u32>();
        return {};
    }

    Result ISelfController::ReportUserIsActive(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // idle:sys is not exposed by the current service stack. There is no guest-visible
        // state to update here, matching the compatibility behavior used by Ryujinx.
        return {};
    }

    Result ISelfController::GetCurrentIlluminance(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<float>(DefaultIlluminanceLux);
        return {};
    }

    Result ISelfController::IsIlluminanceAvailable(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(true);
        return {};
    }

    Result ISelfController::SetAutoSleepDisabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        autoSleepDisabled = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::IsAutoSleepDisabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(autoSleepDisabled);
        return {};
    }

    Result ISelfController::ReportMultimediaError(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto resultCode{request.Pop<Result>()};
        if (request.inputBuf.empty() || request.inputBuf.at(0).size_bytes() < MultimediaTelemetryReportSize)
            return self_controller_result::InvalidParameters;

        LOGW("Guest reported multimedia error 0x{:X}", resultCode.raw);
        return {};
    }

    Result ISelfController::GetCurrentIlluminanceEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // CMIF serializes the bool field in a 32-bit slot here.
        response.Push<u32>(1);
        response.Push<float>(DefaultIlluminanceLux);
        return {};
    }

    Result ISelfController::SetInputDetectionPolicy(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto policy{request.Pop<u32>()};
        if (policy > 1)
            return self_controller_result::InvalidParameters;

        inputDetectionPolicy = policy;
        return {};
    }

    Result ISelfController::SetWirelessPriorityMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto mode{request.Pop<i32>()};
        if (mode < 0 || mode > 1)
            return self_controller_result::InvalidParameters;

        wirelessPriorityMode = mode;
        return {};
    }

    Result ISelfController::GetAccumulatedSuspendedTickValue(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // The emulator does not currently suspend the guest process through AM.
        response.Push<u64>(0);
        return {};
    }

    Result ISelfController::GetAccumulatedSuspendedTickChangedEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(accumulatedSuspendedTickChangedEvent)};
        LOGD("Accumulated Suspended Tick Event Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }

    Result ISelfController::SetAlbumImageTakenNotificationEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        albumImageTakenNotificationEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::SetApplicationAlbumUserData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (request.inputBuf.empty())
            return self_controller_result::InvalidParameters;

        auto input{request.inputBuf.at(0)};
        if (input.size_bytes() > applicationAlbumUserData.size())
            return self_controller_result::InvalidParameters;

        applicationAlbumUserData.fill(0);
        std::copy(input.begin(), input.end(), applicationAlbumUserData.begin());
        applicationAlbumUserDataSize = static_cast<u32>(input.size_bytes());
        return {};
    }

    Result ISelfController::SaveCurrentScreenshot(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto albumReportOption{request.Pop<i32>()};
        if (albumReportOption < 0 || albumReportOption > 3)
            return self_controller_result::InvalidParameters;

        // Screenshot capture is provided by the host UI rather than AM in the current frontend.
        return {};
    }

    Result ISelfController::SetRecordVolumeMuted(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        recordVolumeMuted = request.Pop<u8>() != 0;
        return {};
    }

    Result ISelfController::GetDebugStorageChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Official HOS only exposes this when am.debug!dev_function is enabled. Strato has no
        // equivalent debug setting or IStorageChannel service, so the feature is unavailable.
        return self_controller_result::NotAvailable;
    }
}
