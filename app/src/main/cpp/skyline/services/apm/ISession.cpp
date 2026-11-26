// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "ISession.h"
#include "IManager.h"

namespace skyline::service::apm {
    ISession::ISession(const DeviceState &state, ServiceManager &manager, std::shared_ptr<PerformanceState> performanceState)
        : BaseService(state, manager), performanceState(std::move(performanceState)) {}

    Result ISession::SetPerformanceConfiguration(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto mode{static_cast<PerformanceMode>(request.Pop<i32>())};
        const auto configuration{static_cast<PerformanceConfiguration>(request.Pop<u32>())};

        if (!performanceState->SetPerformanceConfiguration(mode, configuration))
            return result::InvalidParameters;

        LOGD("APM performance configuration: mode={}, config=0x{:08X}", static_cast<i32>(mode), static_cast<u32>(configuration));
        return {};
    }

    Result ISession::GetPerformanceConfiguration(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto mode{static_cast<PerformanceMode>(request.Pop<i32>())};
        const auto configuration{performanceState->GetPerformanceConfiguration(mode)};
        if (!configuration)
            return result::InvalidParameters;

        response.Push<u32>(static_cast<u32>(*configuration));
        return {};
    }

    Result ISession::SetCpuOverclockEnabled(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        performanceState->SetCpuOverclockEnabled(request.Pop<bool>());
        return {};
    }
}
