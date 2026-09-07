// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/settings.h>
#include <services/AutoStubService.h>
#include "IUserInterface.h"

namespace skyline::service::sm {
    IUserInterface::IUserInterface(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IUserInterface::Initialize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IUserInterface::GetService(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto name{request.Pop<ServiceName>()};

        if (!name)
            return result::InvalidServiceName;

        try {
            manager.NewService(name, session, response);
            return {};
        } catch (std::out_of_range &) {
            std::string stringName(span(reinterpret_cast<char *>(&name), sizeof(u64)).as_string(true));

            if (*state.settings->autoStub) {
                auto serviceObject{std::make_shared<AutoStubService>(state, manager, stringName)};
                manager.RegisterService(serviceObject, session, response);
                LOGW("AUTO-STUB: created generic service '{}' -> Success", stringName);
                return {};
            }

            LOGW("Service has not been implemented: \"{}\"", stringName);
            return result::InvalidServiceName;
        }
    }
}
