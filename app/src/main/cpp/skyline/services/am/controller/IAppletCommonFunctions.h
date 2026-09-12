// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include <services/am/applet_state.h>

namespace skyline::service::am {
    /**
     * @brief Common applet functions with behavior implemented only where a concrete
     * modern reference exists.
     * @url https://switchbrew.org/wiki/Applet_Manager_services#IAppletCommonFunctions
     */
    class IAppletCommonFunctions : public BaseService {
      private:
        std::shared_ptr<AppletState> appletState;

      public:
        IAppletCommonFunctions(const DeviceState &state, ServiceManager &manager, std::shared_ptr<AppletState> appletState);

        Result SetDisplayMagnification(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetHomeButtonDoubleClickEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetHomeButtonDoubleClickEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetCpuBoostRequestPriority(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetCurrentApplicationId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetGpuTimeSliceBoost(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result Unknown350(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(42, IAppletCommonFunctions, SetDisplayMagnification),
            SFUNC(50, IAppletCommonFunctions, SetHomeButtonDoubleClickEnabled),
            SFUNC(51, IAppletCommonFunctions, GetHomeButtonDoubleClickEnabled),
            SFUNC(70, IAppletCommonFunctions, SetCpuBoostRequestPriority),
            SFUNC(300, IAppletCommonFunctions, GetCurrentApplicationId),
            SFUNC(320, IAppletCommonFunctions, SetGpuTimeSliceBoost),
            SFUNC(350, IAppletCommonFunctions, Unknown350)
        )
    };
}
