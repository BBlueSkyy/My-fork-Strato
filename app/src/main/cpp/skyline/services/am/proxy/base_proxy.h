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
        BaseProxy(const DeviceState &state, ServiceManager &manager, u64 appletResourceUserId);

        Result GetCommonStateGetter(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetSelfController(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetWindowController(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetAudioController(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetDisplayController(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetProcessWindingController(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetLibraryAppletCreator(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetDebugFunctions(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetAppletCommonFunctions(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
    };
}
