// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <kernel/types/KEvent.h>
#include <services/serviceman.h>
#include <services/am/applet_state.h>
#include <common/macros.h>

namespace skyline::service::am {
    namespace result {
        constexpr Result NoMessages(128, 3);
        constexpr Result InvalidParameters(128, 506);
    }

    /**
     * @brief https://switchbrew.org/wiki/Applet_Manager_services#ICommonStateGetter
     */
    class ICommonStateGetter : public BaseService {
      private:
        enum class Message : u32 {
            ExitRequested = 0x4,
            FocusStateChange = 0xF,
            ExecutionResumed = 0x10,
            OperationModeChange = 0x1E,
            PerformanceModeChange = 0x1F,
            RequestToDisplay = 0x33,
            CaptureButtonShortPressed = 0x5A,
            ScreenshotTaken = 0x5C,
        };

        enum class FocusState : u8 {
            InFocus = 1,
            OutOfFocus = 2,
        };

        enum class OperationMode : u8 {
            Handheld = 0,
            Docked = 1,
        };

        enum class CpuBoostMode : u32 {
            Normal = 0,
            FastLoad = 1,
            PowerSaving = 2
        };

        ENUM_STRING(CpuBoostMode, {
            ENUM_CASE_PAIR(Normal, "Normal");
            ENUM_CASE_PAIR(FastLoad, "Fast Load");
            ENUM_CASE_PAIR(PowerSaving, "Power Saving");
        })

        std::shared_ptr<AppletState> appletState;

      public:
        ICommonStateGetter(const DeviceState &state, ServiceManager &manager, std::shared_ptr<AppletState> appletState);

        Result GetEventHandle(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result ReceiveMessage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetOperationMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetPerformanceMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetBootMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetCurrentFocusState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result RequestToAcquireSleepLock(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result ReleaseSleepLock(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result ReleaseSleepLockTransiently(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetAcquiredSleepLockEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetWakeupCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsVrModeEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetVrModeEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetLcdBacklighOffEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result BeginVrModeEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result EndVrModeEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsInControllerFirmwareUpdateSection(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetDefaultDisplayResolution(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetDefaultDisplayResolutionChangeEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetHdcpAuthenticationState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetHdcpAuthenticationStateChangeEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetCpuBoostMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result CancelCpuBoostMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetBuiltInDisplayType(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsSleepEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsDisablingSleepSuppressed(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result BeginVrMode3d(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result EndVrMode3d(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsVrModeEnabled3d(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, ICommonStateGetter, GetEventHandle),
            SFUNC(0x1, ICommonStateGetter, ReceiveMessage),
            SFUNC(0x5, ICommonStateGetter, GetOperationMode),
            SFUNC(0x6, ICommonStateGetter, GetPerformanceMode),
            SFUNC(0x8, ICommonStateGetter, GetBootMode),
            SFUNC(0x9, ICommonStateGetter, GetCurrentFocusState),
            SFUNC(0xA, ICommonStateGetter, RequestToAcquireSleepLock),
            SFUNC(0xB, ICommonStateGetter, ReleaseSleepLock),
            SFUNC(0xC, ICommonStateGetter, ReleaseSleepLockTransiently),
            SFUNC(0xD, ICommonStateGetter, GetAcquiredSleepLockEvent),
            SFUNC(0xE, ICommonStateGetter, GetWakeupCount),
            SFUNC(0x32, ICommonStateGetter, IsVrModeEnabled),
            SFUNC(0x33, ICommonStateGetter, SetVrModeEnabled),
            SFUNC(0x34, ICommonStateGetter, SetLcdBacklighOffEnabled),
            SFUNC(0x35, ICommonStateGetter, BeginVrModeEx),
            SFUNC(0x36, ICommonStateGetter, EndVrModeEx),
            SFUNC(0x37, ICommonStateGetter, IsInControllerFirmwareUpdateSection),
            SFUNC(0x3C, ICommonStateGetter, GetDefaultDisplayResolution),
            SFUNC(0x3D, ICommonStateGetter, GetDefaultDisplayResolutionChangeEvent),
            SFUNC(0x3E, ICommonStateGetter, GetHdcpAuthenticationState),
            SFUNC(0x3F, ICommonStateGetter, GetHdcpAuthenticationStateChangeEvent),
            SFUNC(0x42, ICommonStateGetter, SetCpuBoostMode),
            SFUNC(0x43, ICommonStateGetter, CancelCpuBoostMode),
            SFUNC(0x44, ICommonStateGetter, GetBuiltInDisplayType),
            SFUNC(0x1F6, ICommonStateGetter, IsSleepEnabled),
            SFUNC(0x1F7, ICommonStateGetter, IsDisablingSleepSuppressed),
            SFUNC(0x384, ICommonStateGetter, SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled),
            SFUNC(0x3E8, ICommonStateGetter, BeginVrMode3d),
            SFUNC(0x3E9, ICommonStateGetter, EndVrMode3d),
            SFUNC(0x3EA, ICommonStateGetter, IsVrModeEnabled3d)
        )
    };
}
