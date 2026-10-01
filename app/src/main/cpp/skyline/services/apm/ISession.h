// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include "performance_state.h"

namespace skyline::service::apm {
    class ISession : public BaseService {
      private:
        std::shared_ptr<PerformanceState> performanceState;

      public:
        ISession(const DeviceState &state, ServiceManager &manager, std::shared_ptr<PerformanceState> performanceState);

        Result SetPerformanceConfiguration(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetPerformanceConfiguration(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetCpuOverclockEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, ISession, SetPerformanceConfiguration),
            SFUNC(0x1, ISession, GetPerformanceConfiguration),
            SFUNC(0x2, ISession, SetCpuOverclockEnabled)
        )
    };
}
