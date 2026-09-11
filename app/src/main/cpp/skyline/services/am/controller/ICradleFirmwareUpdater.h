// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <kernel/types/KEvent.h>
#include <services/serviceman.h>

namespace skyline::service::am {
    class ICradleFirmwareUpdater : public BaseService {
      private:
        std::shared_ptr<kernel::type::KEvent> deviceInfoChangeEvent;

      public:
        ICradleFirmwareUpdater(const DeviceState &state, ServiceManager &manager);

        Result StartUpdate(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result FinishUpdate(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetCradleDeviceInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetCradleDeviceInfoChangeEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

        SERVICE_DECL(
            SFUNC(0, ICradleFirmwareUpdater, StartUpdate),
            SFUNC(1, ICradleFirmwareUpdater, FinishUpdate),
            SFUNC(2, ICradleFirmwareUpdater, GetCradleDeviceInfo),
            SFUNC(3, ICradleFirmwareUpdater, GetCradleDeviceInfoChangeEvent)
        )
    };
}
