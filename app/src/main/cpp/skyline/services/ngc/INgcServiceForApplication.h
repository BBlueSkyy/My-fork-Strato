// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>

namespace skyline::service::ngc {
    /**
     * @brief ngc:u service (Nintendo's no-good-content/word filtering service).
     * @url https://switchbrew.org/wiki/NGC_services#ngc:u
     * @note Only GetContentVersion is implemented. Word checking/masking require
     *       dictionary data and must not silently report success.
     */
    class INgcServiceForApplication : public BaseService {
      public:
        INgcServiceForApplication(const DeviceState &state, ServiceManager &manager);

        Result GetContentVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0, INgcServiceForApplication, GetContentVersion)
        )
    };
}
