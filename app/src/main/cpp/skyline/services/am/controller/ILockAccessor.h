// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <kernel/types/KEvent.h>
#include <kernel/types/KProcess.h>
#include <services/serviceman.h>

namespace skyline::service::am {
    class ILockAccessor : public BaseService {
      private:
        std::mutex mutex;
        bool locked{};
        std::shared_ptr<kernel::type::KEvent> event;

      public:
        ILockAccessor(const DeviceState &state, ServiceManager &manager)
            : BaseService(state, manager), event(std::make_shared<kernel::type::KEvent>(state, true)) {}

        Result TryLock(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
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

        Result Unlock(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
            {
                std::scoped_lock lock{mutex};
                locked = false;
            }
            event->Signal();
            return {};
        }

        Result GetEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
            response.copyHandles.push_back(state.process->InsertItem(event));
            return {};
        }

        Result IsLocked(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
            std::scoped_lock lock{mutex};
            response.Push<u8>(locked);
            return {};
        }

        SERVICE_DECL(
            SFUNC(1, ILockAccessor, TryLock),
            SFUNC(2, ILockAccessor, Unlock),
            SFUNC(3, ILockAccessor, GetEvent),
            SFUNC(4, ILockAccessor, IsLocked)
        )
    };
}
