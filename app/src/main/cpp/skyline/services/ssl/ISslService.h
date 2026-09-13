// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <unordered_map>

#include <services/serviceman.h>

#include "state.h"

namespace skyline::service::ssl {
    /**
     * @brief The root HOS SSL service. ssl and ssl:s share backend state, while
     *        preserving their separate command permissions.
     */
    class ISslService : public BaseService {
        struct SessionState {
            u32 interfaceVersion{};
            bool allowDisableVerifyOption{};
            bool tls12FallbackCleared{};
        };

        std::shared_ptr<SslSharedState> sharedState;
        ServicePermission permission;
        std::mutex sessionStateMutex;
        std::unordered_map<const type::KSession *, SessionState> sessionStates;

        SessionState GetSessionState(const type::KSession &session);
        Result CreateContextImpl(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response, bool systemContext);
        Result UnsupportedModernCommand(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

      public:
        ISslService(const DeviceState &state, ServiceManager &manager, std::shared_ptr<SslSharedState> sharedState, ServicePermission permission);

        Result CreateContext(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetContextCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetCertificates(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetCertificateBufSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result DebugIoctl(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetInterfaceVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result FlushSessionCache(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetDebugOption(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetDebugOption(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result ClearTls12FallbackFlag(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result CreateContextForSystem(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetThreadCoreMask(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetThreadCoreMask(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result VerifySignature(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0, ISslService, CreateContext),
            SFUNC(1, ISslService, GetContextCount),
            SFUNC(2, ISslService, GetCertificates),
            SFUNC(3, ISslService, GetCertificateBufSize),
            SFUNC(4, ISslService, DebugIoctl),
            SFUNC(5, ISslService, SetInterfaceVersion),
            SFUNC(6, ISslService, FlushSessionCache),
            SFUNC(7, ISslService, SetDebugOption),
            SFUNC(8, ISslService, GetDebugOption),
            SFUNC(9, ISslService, ClearTls12FallbackFlag),
            SFUNC(10, ISslService, UnsupportedModernCommand),
            SFUNC(11, ISslService, UnsupportedModernCommand),
            SFUNC(100, ISslService, CreateContextForSystem),
            SFUNC(101, ISslService, SetThreadCoreMask),
            SFUNC(102, ISslService, GetThreadCoreMask),
            SFUNC(103, ISslService, VerifySignature)
        )
    };
}
