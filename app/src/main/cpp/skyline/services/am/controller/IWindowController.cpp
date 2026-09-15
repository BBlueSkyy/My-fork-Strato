// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "IWindowController.h"

namespace skyline::service::am {
    IWindowController::IWindowController(const DeviceState &state, ServiceManager &manager,
                                         std::shared_ptr<AppletState> appletState)
        : BaseService(state, manager), appletState(std::move(appletState)) {}

    Result IWindowController::GetAppletResourceUserId(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u64>(appletState->appletResourceUserId);
        return {};
    }

    Result IWindowController::GetAppletResourceUserIdOfCallerApplet(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        // The current Strato applet frontend model does not create a separate caller process.
        response.Push<u64>(0);
        return {};
    }

    Result IWindowController::AcquireForegroundRights(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->foregroundRightsAcquired = true;
        return {};
    }

    Result IWindowController::ReleaseForegroundRights(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->foregroundRightsAcquired = false;
        return {};
    }

    Result IWindowController::RejectToChangeIntoBackground(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->rejectToChangeIntoBackground = true;
        return {};
    }

    Result IWindowController::SetAppletWindowVisibility(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->appletWindowVisible = request.Pop<u8>() != 0;
        return {};
    }

    Result IWindowController::SetAppletGpuTimeSlice(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->appletGpuTimeSlice = request.Pop<i64>();
        return {};
    }
}
