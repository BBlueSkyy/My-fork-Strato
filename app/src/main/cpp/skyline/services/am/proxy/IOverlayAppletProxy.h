// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
#pragma once
#include "base_proxy.h"
namespace skyline::service::am {
    class IOverlayAppletProxy : public BaseProxy {
      public:
        IOverlayAppletProxy(const DeviceState &state, ServiceManager &manager);
        IOverlayAppletProxy(const DeviceState &state, ServiceManager &manager, u64 appletResourceUserId);
        SERVICE_DECL(
            SFUNC(0x0, BaseProxy, GetCommonStateGetter),
            SFUNC(0x1, BaseProxy, GetSelfController),
            SFUNC(0x2, BaseProxy, GetWindowController),
            SFUNC(0x3, BaseProxy, GetAudioController),
            SFUNC(0x4, BaseProxy, GetDisplayController),
            SFUNC(0xB, BaseProxy, GetLibraryAppletCreator),
            SFUNC(0x15, BaseProxy, GetAppletCommonFunctions),
            SFUNC(0x3E8, BaseProxy, GetDebugFunctions)
        )
    };
}
