// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include "performance_state.h"

namespace skyline::service::apm {
    namespace result {
        constexpr Result InvalidParameters(148, 1);
    }

    class IManager : public BaseService {
      private:
        std::shared_ptr<PerformanceState> performanceState;

      public:
        IManager(const DeviceState &state, ServiceManager &manager, std::shared_ptr<PerformanceState> performanceState);

        Result OpenSession(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetPerformanceMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsCpuOverclockEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, IManager, OpenSession),
            SFUNC(0x1, IManager, GetPerformanceMode),
            SFUNC(0x6, IManager, IsCpuOverclockEnabled)
        )
    };
}
