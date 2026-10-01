// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "ISession.h"
#include "IManager.h"

namespace skyline::service::apm {
    IManager::IManager(const DeviceState &state, ServiceManager &manager, std::shared_ptr<PerformanceState> performanceState)
        : BaseService(state, manager), performanceState(std::move(performanceState)) {}

    Result IManager::OpenSession(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        manager.RegisterService(std::make_shared<ISession>(state, manager, performanceState), session, response);
        return {};
    }

    Result IManager::GetPerformanceMode(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<i32>(static_cast<i32>(performanceState->GetCurrentPerformanceMode()));
        return {};
    }

    Result IManager::IsCpuOverclockEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push(performanceState->IsCpuOverclockEnabled());
        return {};
    }
}
