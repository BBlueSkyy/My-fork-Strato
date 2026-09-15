// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>

namespace skyline::service::account {
    class IAuthorizationRequest : public BaseService {
      public:
        IAuthorizationRequest(const DeviceState &state, ServiceManager &manager);

        Result IsAuthorized(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x13, IAuthorizationRequest, IsAuthorized)
        )
    };
}
