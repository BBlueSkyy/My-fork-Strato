// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once
#include "base_proxy.h"

namespace skyline::service::am {
    class ILibraryAppletProxy : public BaseProxy {
      public:
        ILibraryAppletProxy(const DeviceState &state, ServiceManager &manager);
        ILibraryAppletProxy(const DeviceState &state, ServiceManager &manager, u64 appletResourceUserId);

        Result OpenLibraryAppletSelfAccessor(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

        SERVICE_DECL(
            SFUNC_BASE(0x0, ILibraryAppletProxy, BaseProxy, GetCommonStateGetter),
            SFUNC_BASE(0x1, ILibraryAppletProxy, BaseProxy, GetSelfController),
            SFUNC_BASE(0x2, ILibraryAppletProxy, BaseProxy, GetWindowController),
            SFUNC_BASE(0x3, ILibraryAppletProxy, BaseProxy, GetAudioController),
            SFUNC_BASE(0x4, ILibraryAppletProxy, BaseProxy, GetDisplayController),
            SFUNC_BASE(0xB, ILibraryAppletProxy, BaseProxy, GetLibraryAppletCreator),
            SFUNC(0x14, ILibraryAppletProxy, OpenLibraryAppletSelfAccessor),
            SFUNC_BASE(0x15, ILibraryAppletProxy, BaseProxy, GetAppletCommonFunctions),
            SFUNC_BASE(0x3E8, ILibraryAppletProxy, BaseProxy, GetDebugFunctions)
        )
    };
}
