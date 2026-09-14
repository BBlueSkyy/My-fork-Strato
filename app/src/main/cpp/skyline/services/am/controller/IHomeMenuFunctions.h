// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <services/am/applet_state.h>
#include <services/serviceman.h>

namespace skyline::service::am {
    class IHomeMenuFunctions : public BaseService {
      private:
        std::shared_ptr<AppletState> appletState;

      public:
        IHomeMenuFunctions(const DeviceState &state, ServiceManager &manager,
                           std::shared_ptr<AppletState> appletState);

        Result RequestToGetForeground(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result LockForeground(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result UnlockForeground(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PopFromGeneralChannel(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetPopFromGeneralChannelEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result IsSleepEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result IsRebootEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result IsForceTerminateApplicationDisabledForDebug(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

        SERVICE_DECL(
            SFUNC(10, IHomeMenuFunctions, RequestToGetForeground),
            SFUNC(11, IHomeMenuFunctions, LockForeground),
            SFUNC(12, IHomeMenuFunctions, UnlockForeground),
            SFUNC(20, IHomeMenuFunctions, PopFromGeneralChannel),
            SFUNC(21, IHomeMenuFunctions, GetPopFromGeneralChannelEvent),
            SFUNC(40, IHomeMenuFunctions, IsSleepEnabled),
            SFUNC(41, IHomeMenuFunctions, IsRebootEnabled),
            SFUNC(110, IHomeMenuFunctions, IsForceTerminateApplicationDisabledForDebug)
        )
    };
}
