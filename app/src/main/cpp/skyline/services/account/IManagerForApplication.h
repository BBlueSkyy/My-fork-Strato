// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include "IAccountServiceForApplication.h"

namespace skyline::service::account {
    class IManagerForApplication : public BaseService {
      private:
        static constexpr size_t NasUserBaseForApplicationSize{0x68};
        static constexpr size_t ModernIdTokenCacheSize{0x100};

        UserId userId;
        std::shared_ptr<std::vector<UserId>> openedUsers;

        u64 GetNetworkServiceAccountId() const;
        Result WriteIdTokenCache(ipc::IpcRequest &request, ipc::IpcResponse &response, size_t size);

      public:
        IManagerForApplication(const DeviceState &state, ServiceManager &manager, UserId userId, std::shared_ptr<std::vector<UserId>> openedUsers);

        Result CheckAvailability(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetAccountId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result EnsureIdTokenCacheAsync(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result LoadIdTokenCacheDeprecated(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result LoadIdTokenCache(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetNintendoAccountUserResourceCacheForApplication(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result CreateAuthorizationRequest(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result StoreOpenContext(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, IManagerForApplication, CheckAvailability),
            SFUNC(0x1, IManagerForApplication, GetAccountId),
            SFUNC(0x2, IManagerForApplication, EnsureIdTokenCacheAsync),
            SFUNC(0x3, IManagerForApplication, LoadIdTokenCacheDeprecated),
            SFUNC(0x4, IManagerForApplication, LoadIdTokenCache),
            SFUNC(0x82, IManagerForApplication, GetNintendoAccountUserResourceCacheForApplication),
            SFUNC(0x96, IManagerForApplication, CreateAuthorizationRequest),
            SFUNC(0xA0, IManagerForApplication, StoreOpenContext)
        )
    };
}
