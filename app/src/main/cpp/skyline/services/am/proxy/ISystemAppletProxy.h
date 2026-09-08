// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "base_proxy.h"

namespace skyline::service::am {
    class ISystemAppletProxy : public BaseProxy {
      public:
        ISystemAppletProxy(const DeviceState &state, ServiceManager &manager, u64 appletResourceUserId);

        SERVICE_DECL(
            SFUNC_BASE(0, ISystemAppletProxy, BaseProxy, GetCommonStateGetter),
            SFUNC_BASE(1, ISystemAppletProxy, BaseProxy, GetSelfController),
            SFUNC_BASE(2, ISystemAppletProxy, BaseProxy, GetWindowController),
            SFUNC_BASE(3, ISystemAppletProxy, BaseProxy, GetAudioController),
            SFUNC_BASE(4, ISystemAppletProxy, BaseProxy, GetDisplayController),
            SFUNC_BASE(10, ISystemAppletProxy, BaseProxy, GetProcessWindingController),
            SFUNC_BASE(11, ISystemAppletProxy, BaseProxy, GetLibraryAppletCreator),
            SFUNC_BASE(20, ISystemAppletProxy, BaseProxy, GetHomeMenuFunctions),
            SFUNC_BASE(21, ISystemAppletProxy, BaseProxy, GetGlobalStateController),
            SFUNC_BASE(23, ISystemAppletProxy, BaseProxy, GetAppletCommonFunctions),
            SFUNC_BASE(1000, ISystemAppletProxy, BaseProxy, GetDebugFunctions)
        )
    };
}
