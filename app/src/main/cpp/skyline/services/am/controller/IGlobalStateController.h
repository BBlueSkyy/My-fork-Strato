// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <services/am/applet_state.h>
#include <services/serviceman.h>

namespace skyline::service::am {
    class IGlobalStateController : public BaseService {
      private:
        std::shared_ptr<AppletState> appletState;

      public:
        IGlobalStateController(const DeviceState &state, ServiceManager &manager,
                               std::shared_ptr<AppletState> appletState);

        Result StartShutdownSequence(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result StartRebootSequence(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result LoadAndApplyIdlePolicySettings(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ShouldSleepOnBoot(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetHdcpAuthenticationFailedEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result OpenCradleFirmwareUpdater(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

        SERVICE_DECL(
            SFUNC(3, IGlobalStateController, StartShutdownSequence),
            SFUNC(4, IGlobalStateController, StartRebootSequence),
            SFUNC(10, IGlobalStateController, LoadAndApplyIdlePolicySettings),
            SFUNC(14, IGlobalStateController, ShouldSleepOnBoot),
            SFUNC(15, IGlobalStateController, GetHdcpAuthenticationFailedEvent),
            SFUNC(30, IGlobalStateController, OpenCradleFirmwareUpdater)
        )
    };
}
