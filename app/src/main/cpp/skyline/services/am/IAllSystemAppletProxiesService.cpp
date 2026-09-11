// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "proxy/ILibraryAppletProxy.h"
#include "proxy/IApplicationProxy.h"
#include "proxy/IOverlayAppletProxy.h"
#include "proxy/ISystemAppletProxy.h"
#include "IAllSystemAppletProxiesService.h"

namespace skyline::service::am {
    IAllSystemAppletProxiesService::IAllSystemAppletProxiesService(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IAllSystemAppletProxiesService::OpenLibraryAppletProxy(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(ILibraryAppletProxy, request.pid), session, response);
        return {};
    }

    Result IAllSystemAppletProxiesService::OpenApplicationProxy(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IApplicationProxy, request.pid), session, response);
        return {};
    }

    Result IAllSystemAppletProxiesService::OpenOverlayAppletProxy(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IOverlayAppletProxy, request.pid), session, response);
        return {};
    }

    Result IAllSystemAppletProxiesService::OpenSystemAppletProxy(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(ISystemAppletProxy, request.pid), session, response);
        return {};
    }
}
