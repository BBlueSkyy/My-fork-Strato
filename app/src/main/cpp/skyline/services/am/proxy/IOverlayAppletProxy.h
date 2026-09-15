// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "base_proxy.h"

namespace skyline::service::am {
    class IOverlayAppletProxy : public BaseProxy {
      public:
        IOverlayAppletProxy(const DeviceState &state, ServiceManager &manager, u64 appletResourceUserId);

        SERVICE_DECL(
            SFUNC_BASE(0, IOverlayAppletProxy, BaseProxy, GetCommonStateGetter),
            SFUNC_BASE(1, IOverlayAppletProxy, BaseProxy, GetSelfController),
            SFUNC_BASE(2, IOverlayAppletProxy, BaseProxy, GetWindowController),
            SFUNC_BASE(3, IOverlayAppletProxy, BaseProxy, GetAudioController),
            SFUNC_BASE(4, IOverlayAppletProxy, BaseProxy, GetDisplayController),
            SFUNC_BASE(10, IOverlayAppletProxy, BaseProxy, GetProcessWindingController),
            SFUNC_BASE(11, IOverlayAppletProxy, BaseProxy, GetLibraryAppletCreator),
            SFUNC_BASE(21, IOverlayAppletProxy, BaseProxy, GetAppletCommonFunctions),
            SFUNC_BASE(23, IOverlayAppletProxy, BaseProxy, GetGlobalStateController),
            SFUNC_BASE(1000, IOverlayAppletProxy, BaseProxy, GetDebugFunctions)
        )
    };
}
