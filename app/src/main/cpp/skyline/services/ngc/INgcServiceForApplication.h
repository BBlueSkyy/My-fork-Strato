// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include "ngc_dictionary.h"

namespace skyline::service::ngc {
    class ContentReader;
    /**
     * @brief ngc:u service (Nintendo's no-good-content/word filtering service).
     * @url https://switchbrew.org/wiki/NGC_services#ngc:u
     * @note Filtering requires the original system dictionaries. Unsupported
     *       input and unavailable data return errors, never a clean result.
     */
    class INgcServiceForApplication : public BaseService {
        std::mutex mutex;
        std::shared_ptr<ContentReader> contentReader;
        dictionary::FileReader readFile;
        std::function<bool()> reloadContent;
        dictionary::Filter filter;

      public:
        INgcServiceForApplication(const DeviceState &state, ServiceManager &manager);
        INgcServiceForApplication(const DeviceState &state, ServiceManager &manager,
                                  dictionary::FileReader reader, std::function<bool()> reload);

        Result GetContentVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result Check(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result Mask(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result Reload(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0, INgcServiceForApplication, GetContentVersion),
            SFUNC(1, INgcServiceForApplication, Check),
            SFUNC(2, INgcServiceForApplication, Mask),
            SFUNC(3, INgcServiceForApplication, Reload)
        )
    };
}
