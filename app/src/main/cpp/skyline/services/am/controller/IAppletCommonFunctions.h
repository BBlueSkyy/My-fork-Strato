// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include <services/am/applet_state.h>

namespace skyline::service::am {
    /**
     * @brief Common stateful functions shared by modern applet proxies.
     * @url https://switchbrew.org/wiki/Applet_Manager_services#IAppletCommonFunctions
     */
    class IAppletCommonFunctions : public BaseService {
      private:
        std::shared_ptr<AppletState> appletState;

      public:
        IAppletCommonFunctions(const DeviceState &state, ServiceManager &manager, std::shared_ptr<AppletState> appletState);

        Result SetTerminateResult(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result ReadThemeStorage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result WriteThemeStorage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result PushToAppletBoundChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result TryPopFromAppletBoundChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetDisplayLogicalResolution(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetDisplayMagnification(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetHomeButtonDoubleClickEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetHomeButtonDoubleClickEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsHomeButtonShortPressedBlocked(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsVrModeCurtainRequired(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsSleepRequiredByHighTemperature(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsSleepRequiredByLowBattery(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetCpuBoostRequestPriority(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetHandlingCaptureButtonShortPressedMessageEnabledForApplet(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetHandlingCaptureButtonLongPressedMessageEnabledForApplet(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetBlockingCaptureButtonInEntireSystem(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetApplicationCoreUsageMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetCurrentApplicationId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsSystemAppletHomeMenu(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetGpuTimeSliceBoost(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetGpuTimeSliceBoostDueToApplication(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetGpuErrorEventForApplet(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, IAppletCommonFunctions, SetTerminateResult),
            SFUNC(0xA, IAppletCommonFunctions, ReadThemeStorage),
            SFUNC(0xB, IAppletCommonFunctions, WriteThemeStorage),
            SFUNC(0x14, IAppletCommonFunctions, PushToAppletBoundChannel),
            SFUNC(0x15, IAppletCommonFunctions, TryPopFromAppletBoundChannel),
            SFUNC(0x28, IAppletCommonFunctions, GetDisplayLogicalResolution),
            SFUNC(0x2A, IAppletCommonFunctions, SetDisplayMagnification),
            SFUNC(0x32, IAppletCommonFunctions, SetHomeButtonDoubleClickEnabled),
            SFUNC(0x33, IAppletCommonFunctions, GetHomeButtonDoubleClickEnabled),
            SFUNC(0x34, IAppletCommonFunctions, IsHomeButtonShortPressedBlocked),
            SFUNC(0x3C, IAppletCommonFunctions, IsVrModeCurtainRequired),
            SFUNC(0x3D, IAppletCommonFunctions, IsSleepRequiredByHighTemperature),
            SFUNC(0x3E, IAppletCommonFunctions, IsSleepRequiredByLowBattery),
            SFUNC(0x46, IAppletCommonFunctions, SetCpuBoostRequestPriority),
            SFUNC(0x50, IAppletCommonFunctions, SetHandlingCaptureButtonShortPressedMessageEnabledForApplet),
            SFUNC(0x51, IAppletCommonFunctions, SetHandlingCaptureButtonLongPressedMessageEnabledForApplet),
            SFUNC(0x52, IAppletCommonFunctions, SetBlockingCaptureButtonInEntireSystem),
            SFUNC(0x64, IAppletCommonFunctions, SetApplicationCoreUsageMode),
            SFUNC(0x12C, IAppletCommonFunctions, GetCurrentApplicationId),
            SFUNC(0x136, IAppletCommonFunctions, IsSystemAppletHomeMenu),
            SFUNC(0x140, IAppletCommonFunctions, SetGpuTimeSliceBoost),
            SFUNC(0x141, IAppletCommonFunctions, SetGpuTimeSliceBoostDueToApplication),
            SFUNC(0x172, IAppletCommonFunctions, GetGpuErrorEventForApplet)
        )
    };
}
