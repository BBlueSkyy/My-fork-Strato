// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "ILibraryAppletProxy.h"

namespace skyline::service::am {
    ILibraryAppletProxy::ILibraryAppletProxy(const DeviceState &state, ServiceManager &manager, u64 appletResourceUserId)
        : BaseProxy(state, manager, appletResourceUserId) {}
}
