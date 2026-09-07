// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "IStaticService.h"
#include "IDatabaseService.h"

namespace skyline::service::mii {
    IStaticService::IStaticService(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IStaticService::GetDatabaseService(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // The client supplies the database/service type as a u32. The working APK
        // consumes and preserves it when constructing IDatabaseService.
        const auto databaseType{request.Pop<u32>()};
        manager.RegisterService(SRVREG(IDatabaseService, databaseType), session, response);
        return {};
    }
}
