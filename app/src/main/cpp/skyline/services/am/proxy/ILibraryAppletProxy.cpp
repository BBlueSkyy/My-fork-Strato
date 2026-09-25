// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
#include <services/am/applet/ILibraryAppletSelfAccessor.h>
#include "ILibraryAppletProxy.h"
namespace skyline::service::am {
    ILibraryAppletProxy::ILibraryAppletProxy(const DeviceState &state, ServiceManager &manager) : BaseProxy(state, manager) {}
    ILibraryAppletProxy::ILibraryAppletProxy(const DeviceState &state, ServiceManager &manager, u64 appletResourceUserId) : BaseProxy(state, manager, appletResourceUserId) {}

    Result ILibraryAppletProxy::OpenLibraryAppletSelfAccessor(
        type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (!appletState->nativeAppletContext)
            return Result{128, 500};
        manager.RegisterService(SRVREG(ILibraryAppletSelfAccessor, appletState), session, response);
        return {};
    }
}
