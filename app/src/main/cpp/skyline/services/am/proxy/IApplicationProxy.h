// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "base_proxy.h"

namespace skyline::service::am {
    class IApplicationProxy : public BaseProxy {
      public:
        IApplicationProxy(const DeviceState &state, ServiceManager &manager, u64 appletResourceUserId);

        Result GetApplicationFunctions(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC_BASE(0, IApplicationProxy, BaseProxy, GetCommonStateGetter),
            SFUNC_BASE(1, IApplicationProxy, BaseProxy, GetSelfController),
            SFUNC_BASE(2, IApplicationProxy, BaseProxy, GetWindowController),
            SFUNC_BASE(3, IApplicationProxy, BaseProxy, GetAudioController),
            SFUNC_BASE(4, IApplicationProxy, BaseProxy, GetDisplayController),
            SFUNC_BASE(10, IApplicationProxy, BaseProxy, GetProcessWindingController),
            SFUNC_BASE(11, IApplicationProxy, BaseProxy, GetLibraryAppletCreator),
            SFUNC(20, IApplicationProxy, GetApplicationFunctions),
            SFUNC_BASE(1000, IApplicationProxy, BaseProxy, GetDebugFunctions)
        )
    };
}
