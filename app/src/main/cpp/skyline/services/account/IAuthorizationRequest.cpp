// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "IAuthorizationRequest.h"

namespace skyline::service::account {
    IAuthorizationRequest::IAuthorizationRequest(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IAuthorizationRequest::IsAuthorized(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Strato has no Nintendo authorization backend, so expose the real local state instead of a fake token/code.
        response.Push<u8>(false);
        return {};
    }
}
