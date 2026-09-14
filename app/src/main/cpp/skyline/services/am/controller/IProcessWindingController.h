// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <services/am/applet_state.h>
#include <services/serviceman.h>

namespace skyline::service::am {
    class IProcessWindingController : public BaseService {
      private:
        std::shared_ptr<AppletState> appletState;

      public:
        IProcessWindingController(const DeviceState &state, ServiceManager &manager,
                                  std::shared_ptr<AppletState> appletState);

        Result GetLaunchReason(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result OpenCallingLibraryApplet(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PushContext(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PopContext(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result CancelWindingReservation(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result WindAndDoReserved(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ReserveToStartAndWaitAndUnwindThis(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ReserveToStartAndWait(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

        SERVICE_DECL(
            SFUNC(0, IProcessWindingController, GetLaunchReason),
            SFUNC(11, IProcessWindingController, OpenCallingLibraryApplet),
            SFUNC(21, IProcessWindingController, PushContext),
            SFUNC(22, IProcessWindingController, PopContext),
            SFUNC(23, IProcessWindingController, CancelWindingReservation),
            SFUNC(30, IProcessWindingController, WindAndDoReserved),
            SFUNC(40, IProcessWindingController, ReserveToStartAndWaitAndUnwindThis),
            SFUNC(41, IProcessWindingController, ReserveToStartAndWait)
        )
    };
}
