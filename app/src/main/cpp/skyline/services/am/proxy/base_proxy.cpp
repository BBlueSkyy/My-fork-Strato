// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <services/am/controller/ICommonStateGetter.h>
#include <services/am/controller/ISelfController.h>
#include <services/am/controller/IWindowController.h>
#include <services/am/controller/IAudioController.h>
#include <services/am/controller/IDisplayController.h>
#include <services/am/controller/IProcessWindingController.h>
#include <services/am/controller/ILibraryAppletCreator.h>
#include <services/am/controller/IDebugFunctions.h>
#include <services/am/controller/IAppletCommonFunctions.h>
#include "base_proxy.h"

namespace skyline::service::am {
    BaseProxy::BaseProxy(const DeviceState &state, ServiceManager &manager, u64 appletResourceUserId)
        : BaseService(state, manager), appletState(std::make_shared<AppletState>(state)) {
        appletState->appletResourceUserId = appletResourceUserId;
    }

    Result BaseProxy::GetCommonStateGetter(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(ICommonStateGetter, appletState), session, response);
        return {};
    }

    Result BaseProxy::GetSelfController(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(ISelfController, appletState), session, response);
        return {};
    }

    Result BaseProxy::GetWindowController(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IWindowController, appletState), session, response);
        return {};
    }

    Result BaseProxy::GetAudioController(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IAudioController), session, response);
        return {};
    }

    Result BaseProxy::GetDisplayController(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IDisplayController), session, response);
        return {};
    }

    Result BaseProxy::GetProcessWindingController(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IProcessWindingController, appletState), session, response);
        return {};
    }

    Result BaseProxy::GetLibraryAppletCreator(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(ILibraryAppletCreator, appletState), session, response);
        return {};
    }

    Result BaseProxy::GetDebugFunctions(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IDebugFunctions), session, response);
        return {};
    }

    Result BaseProxy::GetAppletCommonFunctions(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IAppletCommonFunctions, appletState), session, response);
        return {};
    }
}
