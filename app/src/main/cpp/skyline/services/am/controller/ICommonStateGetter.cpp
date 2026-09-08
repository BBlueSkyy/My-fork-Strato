// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/settings.h>
#include <kernel/types/KProcess.h>
#include "ICommonStateGetter.h"

namespace skyline::service::am {
    ICommonStateGetter::ICommonStateGetter(const DeviceState &state, ServiceManager &manager, std::shared_ptr<AppletState> appletState)
        : BaseService(state, manager), appletState(std::move(appletState)) {
        LOGI("Switch to mode: {}", *state.settings->isDocked ? "Docked" : "Handheld");
    }

    Result ICommonStateGetter::GetEventHandle(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(appletState->messageEvent)};
        LOGD("Applet Event Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }

    Result ICommonStateGetter::ReceiveMessage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        u32 message{};
        if (!appletState->PopMessage(message))
            return result::NoMessages;

        response.Push<u32>(message);
        return {};
    }

    Result ICommonStateGetter::GetCurrentFocusState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u8>(static_cast<u8>(appletState->focusState));
        return {};
    }

    Result ICommonStateGetter::GetOperationMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(static_cast<OperationMode>(*state.settings->isDocked));
        return {};
    }

    Result ICommonStateGetter::GetPerformanceMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u32>(*state.settings->isDocked ? 1 : 0);
        return {};
    }

    Result ICommonStateGetter::GetBootMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Strato only emulates a normal retail boot environment.
        response.Push<u8>(0);
        return {};
    }

    Result ICommonStateGetter::RequestToAcquireSleepLock(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        {
            std::scoped_lock lock{appletState->mutex};
            appletState->sleepLockAcquired = true;
        }
        appletState->sleepLockEvent->Signal();
        return {};
    }

    Result ICommonStateGetter::ReleaseSleepLock(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        {
            std::scoped_lock lock{appletState->mutex};
            appletState->sleepLockAcquired = false;
        }
        appletState->sleepLockEvent->ResetSignal();
        return {};
    }

    Result ICommonStateGetter::ReleaseSleepLockTransiently(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // There is no asynchronous power manager in Strato, so transient release is
        // immediately followed by reacquisition while still producing the wake event.
        appletState->sleepLockEvent->ResetSignal();
        appletState->sleepLockEvent->Signal();
        return {};
    }

    Result ICommonStateGetter::GetAcquiredSleepLockEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(appletState->sleepLockEvent)};
        response.copyHandles.push_back(handle);
        return {};
    }

    Result ICommonStateGetter::GetWakeupCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u64>(appletState->wakeupCount);
        return {};
    }

    Result ICommonStateGetter::IsVrModeEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u8>(appletState->vrModeEnabled);
        return {};
    }

    Result ICommonStateGetter::SetVrModeEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->vrModeEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result ICommonStateGetter::SetLcdBacklighOffEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] auto lcdBacklightOffEnabled{request.Pop<u8>()};
        return {};
    }

    Result ICommonStateGetter::BeginVrModeEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->vrModeEnabled = true;
        return {};
    }

    Result ICommonStateGetter::EndVrModeEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->vrModeEnabled = false;
        return {};
    }

    Result ICommonStateGetter::IsInControllerFirmwareUpdateSection(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(false);
        return {};
    }

    Result ICommonStateGetter::GetDefaultDisplayResolution(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!*state.settings->isDocked) {
            response.Push<u32>(1280);
            response.Push<u32>(720);
        } else {
            response.Push<u32>(1920);
            response.Push<u32>(1080);
        }
        return {};
    }

    Result ICommonStateGetter::GetDefaultDisplayResolutionChangeEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(appletState->defaultDisplayResolutionChangeEvent)};
        LOGD("Default Display Resolution Change Event Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }

    Result ICommonStateGetter::GetHdcpAuthenticationState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // 1 is the normal authenticated state used by applications.
        response.Push<i32>(1);
        return {};
    }

    Result ICommonStateGetter::GetHdcpAuthenticationStateChangeEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(appletState->hdcpStateChangeEvent)};
        response.copyHandles.push_back(handle);
        return {};
    }

    Result ICommonStateGetter::SetCpuBoostMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto mode{request.Pop<CpuBoostMode>()};
        switch (mode) {
            case CpuBoostMode::Normal:
            case CpuBoostMode::FastLoad:
            case CpuBoostMode::PowerSaving: {
                std::scoped_lock lock{appletState->mutex};
                appletState->cpuBoostMode = static_cast<u32>(mode);
                LOGI("Set CPU boost mode to {}", ToString(mode));
                return {};
            }
            default:
                LOGE("Unknown CPU boost mode value: 0x{:X}", static_cast<u32>(mode));
                return result::InvalidParameters;
        }
    }

    Result ICommonStateGetter::CancelCpuBoostMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->cpuBoostMode = static_cast<u32>(CpuBoostMode::Normal);
        return {};
    }

    Result ICommonStateGetter::GetBuiltInDisplayType(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // The emulated target is the standard built-in LCD.
        response.Push<i32>(0);
        return {};
    }

    Result ICommonStateGetter::IsSleepEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u8>(!appletState->sleepDisabledTillShutdown);
        return {};
    }

    Result ICommonStateGetter::IsDisablingSleepSuppressed(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u8>(appletState->sleepDisablingSuppressed);
        return {};
    }

    Result ICommonStateGetter::BeginVrMode3d(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->vrMode3dEnabled = true;
        return {};
    }

    Result ICommonStateGetter::EndVrMode3d(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->vrMode3dEnabled = false;
        return {};
    }

    Result ICommonStateGetter::IsVrModeEnabled3d(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u8>(appletState->vrMode3dEnabled);
        return {};
    }

    Result ICommonStateGetter::SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->requestExitToLibraryAppletAtExecuteNextProgramEnabled = request.Pop<u8>() != 0;
        return {};
    }
}
