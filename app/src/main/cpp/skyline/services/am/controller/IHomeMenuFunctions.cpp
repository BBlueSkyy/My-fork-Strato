// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#include <kernel/types/KProcess.h>
#include <services/am/storage/IStorage.h>
#include "IHomeMenuFunctions.h"

namespace skyline::service::am {
    IHomeMenuFunctions::IHomeMenuFunctions(const DeviceState &state, ServiceManager &manager,
                                           std::shared_ptr<AppletState> appletState)
        : BaseService(state, manager), appletState(std::move(appletState)) {}

    Result IHomeMenuFunctions::RequestToGetForeground(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        {
            std::scoped_lock lock{appletState->mutex};
            appletState->focusState = 1;
        }
        appletState->QueueMessage(AppletState::FocusStateChangedMessage);
        return {};
    }

    Result IHomeMenuFunctions::LockForeground(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->homeMenuForegroundLocked = true;
        return {};
    }

    Result IHomeMenuFunctions::UnlockForeground(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{appletState->mutex};
        appletState->homeMenuForegroundLocked = false;
        return {};
    }

    Result IHomeMenuFunctions::PopFromGeneralChannel(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::shared_ptr<IStorage> storage;
        {
            std::scoped_lock lock{appletState->mutex};
            if (appletState->generalChannel.empty())
                return Result{128, 2};
            storage = std::move(appletState->generalChannel.front());
            appletState->generalChannel.pop_front();
            if (appletState->generalChannel.empty())
                appletState->generalChannelEvent->ResetSignal();
        }
        manager.RegisterService(storage, session, response);
        return {};
    }

    Result IHomeMenuFunctions::GetPopFromGeneralChannelEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(appletState->generalChannelEvent));
        return {};
    }

    Result IHomeMenuFunctions::IsSleepEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u8>(false);
        return {};
    }

    Result IHomeMenuFunctions::IsRebootEnabled(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u8>(true);
        return {};
    }

    Result IHomeMenuFunctions::IsForceTerminateApplicationDisabledForDebug(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u8>(false);
        return {};
    }
}
