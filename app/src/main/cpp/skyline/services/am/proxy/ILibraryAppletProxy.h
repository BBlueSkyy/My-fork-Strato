// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "base_proxy.h"

namespace skyline::service::am {
    class ILibraryAppletProxy : public BaseProxy {
      public:
        ILibraryAppletProxy(const DeviceState &state, ServiceManager &manager, u64 appletResourceUserId);

        SERVICE_DECL(
            SFUNC_BASE(0, ILibraryAppletProxy, BaseProxy, GetCommonStateGetter),
            SFUNC_BASE(1, ILibraryAppletProxy, BaseProxy, GetSelfController),
            SFUNC_BASE(2, ILibraryAppletProxy, BaseProxy, GetWindowController),
            SFUNC_BASE(3, ILibraryAppletProxy, BaseProxy, GetAudioController),
            SFUNC_BASE(4, ILibraryAppletProxy, BaseProxy, GetDisplayController),
            SFUNC_BASE(10, ILibraryAppletProxy, BaseProxy, GetProcessWindingController),
            SFUNC_BASE(11, ILibraryAppletProxy, BaseProxy, GetLibraryAppletCreator),
            SFUNC_BASE(21, ILibraryAppletProxy, BaseProxy, GetAppletCommonFunctions),
            SFUNC_BASE(1000, ILibraryAppletProxy, BaseProxy, GetDebugFunctions)
        )
    };
}
