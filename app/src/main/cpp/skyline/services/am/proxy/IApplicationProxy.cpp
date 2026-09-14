// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <os.h>
#include <services/am/controller/IApplicationFunctions.h>
#include <services/am/storage/VectorIStorage.h>
#include "IApplicationProxy.h"

namespace skyline::service::am {
    IApplicationProxy::IApplicationProxy(const DeviceState &state, ServiceManager &manager, u64 appletResourceUserId)
        : BaseProxy(state, manager, appletResourceUserId) {
        appletState->previousProgramIndex = state.os->GetPreviousProgramIndex();
        for (auto &data : state.os->TakeUserChannelLaunchParameters())
            appletState->userChannel.emplace_back(std::make_shared<VectorIStorage>(state, manager, std::move(data)));
    }

    Result IApplicationProxy::GetApplicationFunctions(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IApplicationFunctions, appletState), session, response);
        return {};
    }
}
