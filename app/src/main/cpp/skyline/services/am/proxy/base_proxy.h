// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include <services/am/applet_state.h>

namespace skyline::service::am {
    class BaseProxy : public BaseService {
      protected:
        std::shared_ptr<AppletState> appletState;

      public:
        BaseProxy(const DeviceState &state, ServiceManager &manager);
        BaseProxy(const DeviceState &state, ServiceManager &manager, u64 appletResourceUserId);

        Result GetCommonStateGetter(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetSelfController(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetWindowController(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetAudioController(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetDisplayController(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetLibraryAppletCreator(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetDebugFunctions(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetAppletCommonFunctions(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
    };
}
