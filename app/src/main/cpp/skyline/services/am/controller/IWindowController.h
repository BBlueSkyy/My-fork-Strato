// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/am/applet_state.h>
#include <services/serviceman.h>

namespace skyline::service::am {
    class IWindowController : public BaseService {
      private:
        std::shared_ptr<AppletState> appletState;

      public:
        IWindowController(const DeviceState &state, ServiceManager &manager, std::shared_ptr<AppletState> appletState);

        Result GetAppletResourceUserId(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetAppletResourceUserIdOfCallerApplet(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result AcquireForegroundRights(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ReleaseForegroundRights(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result RejectToChangeIntoBackground(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetAppletWindowVisibility(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetAppletGpuTimeSlice(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

        SERVICE_DECL(
            SFUNC(1, IWindowController, GetAppletResourceUserId),
            SFUNC(2, IWindowController, GetAppletResourceUserIdOfCallerApplet),
            SFUNC(10, IWindowController, AcquireForegroundRights),
            SFUNC(11, IWindowController, ReleaseForegroundRights),
            SFUNC(12, IWindowController, RejectToChangeIntoBackground),
            SFUNC(20, IWindowController, SetAppletWindowVisibility),
            SFUNC(21, IWindowController, SetAppletGpuTimeSlice)
        )
    };
}
