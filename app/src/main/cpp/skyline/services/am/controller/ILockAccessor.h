// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <kernel/types/KEvent.h>
#include <services/serviceman.h>

namespace skyline::service::am {
    class ILockAccessor : public BaseService {
      private:
        std::mutex mutex;
        bool locked{};
        std::shared_ptr<kernel::type::KEvent> event;

      public:
        ILockAccessor(const DeviceState &state, ServiceManager &manager);

        Result TryLock(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result Unlock(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsLocked(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(1, ILockAccessor, TryLock),
            SFUNC(2, ILockAccessor, Unlock),
            SFUNC(3, ILockAccessor, GetEvent),
            SFUNC(4, ILockAccessor, IsLocked)
        )
    };
}
