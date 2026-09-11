// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/am/applet_state.h>
#include <services/serviceman.h>

namespace skyline::service::am {
    class ILibraryAppletCreator : public BaseService {
      private:
        std::shared_ptr<AppletState> appletState;

      public:
        ILibraryAppletCreator(const DeviceState &state, ServiceManager &manager,
                              std::shared_ptr<AppletState> appletState);

        Result CreateLibraryApplet(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result CreateLibraryAppletEx(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result CreateStorage(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result CreateTransferMemoryStorage(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result CreateHandleStorage(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

        SERVICE_DECL(
            SFUNC(0, ILibraryAppletCreator, CreateLibraryApplet),
            SFUNC(3, ILibraryAppletCreator, CreateLibraryAppletEx),
            SFUNC(10, ILibraryAppletCreator, CreateStorage),
            SFUNC(11, ILibraryAppletCreator, CreateTransferMemoryStorage),
            SFUNC(12, ILibraryAppletCreator, CreateHandleStorage)
        )
    };
}
