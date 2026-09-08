// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include <services/am/applet_state.h>
#include <common/macros.h>

namespace skyline::service::am {
    namespace result {
        constexpr Result NoMessages(128, 3);
        constexpr Result InvalidParameters(128, 506);
    }

    class ICommonStateGetter : public BaseService {
      private:
        enum class CpuBoostMode : u32 {
            Normal = 0,
            FastLoad = 1,
            PowerSaving = 2,
        };

        ENUM_STRING(CpuBoostMode, {
            ENUM_CASE_PAIR(Normal, "Normal");
            ENUM_CASE_PAIR(FastLoad, "Fast Load");
            ENUM_CASE_PAIR(PowerSaving, "Power Saving");
        })

        std::shared_ptr<AppletState> appletState;

      public:
        ICommonStateGetter(const DeviceState &state, ServiceManager &manager, std::shared_ptr<AppletState> appletState);

        Result GetEventHandle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ReceiveMessage(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetOperationMode(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetPerformanceMode(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetBootMode(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetCurrentFocusState(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result RequestToAcquireSleepLock(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ReleaseSleepLock(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ReleaseSleepLockTransiently(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetAcquiredSleepLockEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PushToGeneralChannel(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result IsVrModeEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetVrModeEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetLcdBacklighOffEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result BeginVrModeEx(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result EndVrModeEx(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result IsInControllerFirmwareUpdateSection(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetDefaultDisplayResolution(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetDefaultDisplayResolutionChangeEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetHdcpAuthenticationState(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetHdcpAuthenticationStateChangeEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetCpuBoostMode(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetBuiltInDisplayType(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PerformSystemButtonPressingIfInFocus(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetHandlingHomeButtonShortPressedEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result EnableStartupLogoDisappearedMessage(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetOperationModeSystemInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetSettingsPlatformRegion(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result Unknown610(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result Unknown611(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result BeginVrMode3d(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result EndVrMode3d(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result IsVrModeEnabled3d(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetVrLaboGoggleViewport(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetPanelPhysicalSizeForSpecificTitle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetPanelResolutionForSpecificTitle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

        SERVICE_DECL(
            SFUNC(0, ICommonStateGetter, GetEventHandle),
            SFUNC(1, ICommonStateGetter, ReceiveMessage),
            SFUNC(5, ICommonStateGetter, GetOperationMode),
            SFUNC(6, ICommonStateGetter, GetPerformanceMode),
            SFUNC(8, ICommonStateGetter, GetBootMode),
            SFUNC(9, ICommonStateGetter, GetCurrentFocusState),
            SFUNC(10, ICommonStateGetter, RequestToAcquireSleepLock),
            SFUNC(11, ICommonStateGetter, ReleaseSleepLock),
            SFUNC(12, ICommonStateGetter, ReleaseSleepLockTransiently),
            SFUNC(13, ICommonStateGetter, GetAcquiredSleepLockEvent),
            SFUNC(20, ICommonStateGetter, PushToGeneralChannel),
            SFUNC(50, ICommonStateGetter, IsVrModeEnabled),
            SFUNC(51, ICommonStateGetter, SetVrModeEnabled),
            SFUNC(52, ICommonStateGetter, SetLcdBacklighOffEnabled),
            SFUNC(53, ICommonStateGetter, BeginVrModeEx),
            SFUNC(54, ICommonStateGetter, EndVrModeEx),
            SFUNC(55, ICommonStateGetter, IsInControllerFirmwareUpdateSection),
            SFUNC(60, ICommonStateGetter, GetDefaultDisplayResolution),
            SFUNC(61, ICommonStateGetter, GetDefaultDisplayResolutionChangeEvent),
            SFUNC(62, ICommonStateGetter, GetHdcpAuthenticationState),
            SFUNC(63, ICommonStateGetter, GetHdcpAuthenticationStateChangeEvent),
            SFUNC(66, ICommonStateGetter, SetCpuBoostMode),
            SFUNC(68, ICommonStateGetter, GetBuiltInDisplayType),
            SFUNC(80, ICommonStateGetter, PerformSystemButtonPressingIfInFocus),
            SFUNC(100, ICommonStateGetter, SetHandlingHomeButtonShortPressedEnabled),
            SFUNC(130, ICommonStateGetter, EnableStartupLogoDisappearedMessage),
            SFUNC(200, ICommonStateGetter, GetOperationModeSystemInfo),
            SFUNC(300, ICommonStateGetter, GetSettingsPlatformRegion),
            SFUNC(610, ICommonStateGetter, Unknown610),
            SFUNC(611, ICommonStateGetter, Unknown611),
            SFUNC(900, ICommonStateGetter, SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled),
            SFUNC(1000, ICommonStateGetter, BeginVrMode3d),
            SFUNC(1001, ICommonStateGetter, EndVrMode3d),
            SFUNC(1002, ICommonStateGetter, IsVrModeEnabled3d),
            SFUNC(1003, ICommonStateGetter, GetVrLaboGoggleViewport),
            SFUNC(1004, ICommonStateGetter, GetPanelPhysicalSizeForSpecificTitle),
            SFUNC(1005, ICommonStateGetter, GetPanelResolutionForSpecificTitle)
        )
    };
}
