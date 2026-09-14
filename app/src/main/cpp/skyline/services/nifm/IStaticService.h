// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>

namespace skyline::service::nifm {
    /**
     * @brief nn::nifm::detail::IStaticService exposed as nifm:u/nifm:s/nifm:a
     * @url https://switchbrew.org/wiki/Network_Interface_services
     */
    class IStaticService : public BaseService {
      public:
        IStaticService(const DeviceState &state, ServiceManager &manager);

        Result CreateGeneralServiceOld(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result CreateGeneralService(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x4, IStaticService, CreateGeneralServiceOld),
            SFUNC(0x5, IStaticService, CreateGeneralService)
        )
    };
}
