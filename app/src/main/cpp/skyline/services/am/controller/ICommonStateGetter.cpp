// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/settings.h>
#include <cstring>
#include <kernel/types/KProcess.h>
#include <services/am/storage/IStorage.h>
#include "ICommonStateGetter.h"
#include "ILockAccessor.h"

namespace skyline::service::am {
    namespace {
        constexpr u32 AppletMessageFocusStateChanged{15};
        constexpr u32 AppletMessageHomeButtonShort{20};
        constexpr u32 AppletMessageHomeButtonLong{21};
        constexpr u32 AppletMessageCaptureButtonShort{90};
        constexpr u32 AppletMessageCaptureButtonLong{91};
        constexpr u32 AppletMessageStartupLogoDisappeared{95};
    }

    ICommonStateGetter::ICommonStateGetter(const DeviceState &state, ServiceManager &manager,
                                           std::shared_ptr<AppletState> appletState)
        : BaseService(state, manager), appletState(std::move(appletState)) {
        LOGI("Switch to mode: {}", *state.settings->isDocked ? "Docked" : "Handheld");
    }

    Result ICommonStateGetter::GetEventHandle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(appletState->messageEvent));
        return {};
    }

    Result ICommonStateGetter::ReceiveMessage(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        u32 message{};
        if (!appletState->PopMessage(message))
            return result::NoMessages;
        response.Push<u32>(message);
        return {};
    }

    Result ICommonStateGetter::GetOperationMode(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u8>(*state.settings->isDocked ? 1 : 0);
        return {};
    }

    Result ICommonStateGetter::GetPerformanceMode(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(*state.settings->isDocked ? 1 : 0);
        return {};
    }

    Result ICommonStateGetter::GetBootMode(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u8>(0);
        return {};
    }

    Result ICommonStateGetter::GetCurrentFocusState(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u8>(appletState->focusState);
        return {};
    }

    Result ICommonStateGetter::RequestToAcquireSleepLock(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        {
            std::scoped_lock lock{appletState->mutex};
            appletState->sleepLockAcquired = true;
        }
        appletState->sleepLockEvent->Signal();
        return {};
    }

    Result ICommonStateGetter::ReleaseSleepLock(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        {
            std::scoped_lock lock{appletState->mutex};
            appletState->sleepLockAcquired = false;
        }
        appletState->sleepLockEvent->ResetSignal();
        return {};
    }

    Result ICommonStateGetter::ReleaseSleepLockTransiently(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        {
            std::scoped_lock lock{appletState->mutex};
            appletState->sleepLockAcquired = false;
        }
        appletState->sleepLockEvent->ResetSignal();
        return {};
    }

    Result ICommonStateGetter::GetAcquiredSleepLockEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(appletState->sleepLockEvent));
        return {};
    }

    Result ICommonStateGetter::PushToGeneralChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        auto storage{request.PopService<IStorage>(0, session)};
        std::scoped_lock lock{appletState->mutex};
        appletState->generalChannel.emplace_back(std::move(storage));
        return {};
    }

    Result ICommonStateGetter::GetHomeButtonReaderLockAccessor(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(ILockAccessor), session, response);
        return {};
    }

    Result ICommonStateGetter::GetReaderLockAccessorEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const u32 buttonType{request.Pop<u32>()};
        manager.RegisterService(SRVREG(ILockAccessor), session, response);
        return {};
    }

    Result ICommonStateGetter::GetWriterLockAccessorEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const u32 buttonType{request.Pop<u32>()};
        manager.RegisterService(SRVREG(ILockAccessor), session, response);
        return {};
    }

    Result ICommonStateGetter::IsVrModeEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u8>(appletState->vrModeEnabled);
        return {};
    }

    Result ICommonStateGetter::SetVrModeEnabled(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->vrModeEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result ICommonStateGetter::SetLcdBacklighOffEnabled(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        [[maybe_unused]] const bool enabled{request.Pop<u8>() != 0};
        return {};
    }

    Result ICommonStateGetter::BeginVrModeEx(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->vrModeEnabled = true;
        return {};
    }

    Result ICommonStateGetter::EndVrModeEx(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->vrModeEnabled = false;
        return {};
    }

    Result ICommonStateGetter::IsInControllerFirmwareUpdateSection(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u8>(false);
        return {};
    }

    Result ICommonStateGetter::GetDefaultDisplayResolution(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<i32>(*state.settings->isDocked ? 1920 : 1280);
        response.Push<i32>(*state.settings->isDocked ? 1080 : 720);
        return {};
    }

    Result ICommonStateGetter::GetDefaultDisplayResolutionChangeEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(appletState->defaultDisplayResolutionChangeEvent));
        return {};
    }

    Result ICommonStateGetter::GetHdcpAuthenticationState(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<i32>(1);
        return {};
    }

    Result ICommonStateGetter::GetHdcpAuthenticationStateChangeEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(appletState->hdcpStateChangeEvent));
        return {};
    }

    Result ICommonStateGetter::SetCpuBoostMode(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto mode{request.Pop<CpuBoostMode>()};
        if (mode != CpuBoostMode::Normal && mode != CpuBoostMode::FastLoad)
            return result::InvalidParameters;

        std::scoped_lock lock{appletState->mutex};
        appletState->cpuBoostMode = static_cast<u32>(mode);
        return {};
    }

    Result ICommonStateGetter::GetBuiltInDisplayType(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<i32>(0);
        return {};
    }

    Result ICommonStateGetter::PerformSystemButtonPressingIfInFocus(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto type{request.Pop<SystemButtonType>()};
        bool emit{};
        u32 message{};
        {
            std::scoped_lock lock{appletState->mutex};
            if (appletState->focusState != 1)
                return {};

            switch (type) {
                case SystemButtonType::HomeButtonShortPressing:
                    emit = !appletState->homeButtonShortPressedBlocked;
                    message = AppletMessageHomeButtonShort;
                    break;
                case SystemButtonType::HomeButtonLongPressing:
                    emit = !appletState->homeButtonLongPressedBlocked;
                    message = AppletMessageHomeButtonLong;
                    break;
                case SystemButtonType::CaptureButtonShortPressing:
                    emit = appletState->handlingCaptureButtonShortPressedMessageEnabled;
                    message = AppletMessageCaptureButtonShort;
                    break;
                case SystemButtonType::CaptureButtonLongPressing:
                    emit = appletState->handlingCaptureButtonLongPressedMessageEnabled;
                    message = AppletMessageCaptureButtonLong;
                    break;
                default:
                    break;
            }
        }

        if (emit)
            appletState->QueueMessage(message);
        return {};
    }

    Result ICommonStateGetter::GetCurrentPerformanceConfiguration(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(*state.settings->isDocked ? 0x00020001 : 0x00010000);
        return {};
    }

    Result ICommonStateGetter::SetHandlingHomeButtonShortPressedEnabled(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const bool enabled{request.Pop<u8>() != 0};
        std::scoped_lock lock{appletState->mutex};
        appletState->homeButtonShortPressedBlocked = !enabled;
        return {};
    }

    Result ICommonStateGetter::GetAppletLaunchedHistory(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 count{};
        if (!request.outputBuf.empty() && request.outputBuf.at(0).size() >= sizeof(u32)) {
            const u32 appletId{1}; // nn::am::AppletId::Application
            std::memcpy(request.outputBuf.at(0).data(), &appletId, sizeof(appletId));
            count = 1;
        }
        response.Push<i32>(count);
        return {};
    }

    Result ICommonStateGetter::EnableStartupLogoDisappearedMessage(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        appletState->QueueMessage(AppletMessageStartupLogoDisappeared);
        return {};
    }

    Result ICommonStateGetter::GetOperationModeSystemInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(0);
        return {};
    }

    Result ICommonStateGetter::GetSettingsPlatformRegion(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<i32>(1); // Global
        return {};
    }

    Result ICommonStateGetter::Unknown610(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        [[maybe_unused]] const u64 value{request.Pop<u64>()};
        return {};
    }

    Result ICommonStateGetter::Unknown611(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        [[maybe_unused]] const u8 value{request.Pop<u8>()};
        return {};
    }

    Result ICommonStateGetter::SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->requestExitToLibraryAppletAtExecuteNextProgramEnabled = true;
        return {};
    }

    Result ICommonStateGetter::BeginVrMode3d(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->vrMode3dEnabled = true;
        return {};
    }

    Result ICommonStateGetter::EndVrMode3d(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->vrMode3dEnabled = false;
        return {};
    }

    Result ICommonStateGetter::IsVrModeEnabled3d(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u8>(appletState->vrMode3dEnabled);
        return {};
    }

    Result ICommonStateGetter::GetVrLaboGoggleViewport(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<i32>(0);
        response.Push<i32>(0);
        response.Push<i32>(1280);
        response.Push<i32>(720);
        return {};
    }

    Result ICommonStateGetter::GetPanelPhysicalSizeForSpecificTitle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<float>(137.25F);
        response.Push<float>(77.2F);
        return {};
    }

    Result ICommonStateGetter::GetPanelResolutionForSpecificTitle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<i32>(1280);
        response.Push<i32>(720);
        return {};
    }
}
