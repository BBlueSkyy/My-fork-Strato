// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#include <kernel/types/KProcess.h>
#include "ILockAccessor.h"

namespace skyline::service::am {
    ILockAccessor::ILockAccessor(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager), event(std::make_shared<kernel::type::KEvent>(state, true)) {}

    Result ILockAccessor::TryLock(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const bool returnHandle{request.Pop<u8>() != 0};
        bool acquired{};
        {
            std::scoped_lock lock{mutex};
            if (!locked) {
                locked = true;
                acquired = true;
            }
        }

        response.Push<u8>(acquired);
        if (returnHandle)
            response.copyHandles.push_back(state.process->InsertItem(event));
        return {};
    }

    Result ILockAccessor::Unlock(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        {
            std::scoped_lock lock{mutex};
            locked = false;
        }
        event->Signal();
        return {};
    }

    Result ILockAccessor::GetEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(event));
        return {};
    }

    Result ILockAccessor::IsLocked(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{mutex};
        response.Push<u8>(locked);
        return {};
    }
}
