// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#include <kernel/types/KProcess.h>
#include "ICradleFirmwareUpdater.h"
#include "IGlobalStateController.h"

namespace skyline::service::am {
    IGlobalStateController::IGlobalStateController(const DeviceState &state, ServiceManager &manager,
                                                   std::shared_ptr<AppletState> appletState)
        : BaseService(state, manager), appletState(std::move(appletState)) {}

    Result IGlobalStateController::StartShutdownSequence(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->shutdownRequested = true;
        return {};
    }

    Result IGlobalStateController::StartRebootSequence(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->rebootRequested = true;
        return {};
    }

    Result IGlobalStateController::LoadAndApplyIdlePolicySettings(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result IGlobalStateController::ShouldSleepOnBoot(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u8>(false);
        return {};
    }

    Result IGlobalStateController::GetHdcpAuthenticationFailedEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(appletState->hdcpAuthenticationFailedEvent));
        return {};
    }

    Result IGlobalStateController::OpenCradleFirmwareUpdater(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(ICradleFirmwareUpdater), session, response);
        return {};
    }
}
