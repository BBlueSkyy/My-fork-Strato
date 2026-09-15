// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "ISystemAppletProxy.h"

namespace skyline::service::am {
    ISystemAppletProxy::ISystemAppletProxy(const DeviceState &state, ServiceManager &manager, u64 appletResourceUserId)
        : BaseProxy(state, manager, appletResourceUserId) {}
}
