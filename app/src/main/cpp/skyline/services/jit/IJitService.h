// SPDX-License-Identifier: MPL-2.0
// Diagnostic-only stub. No guest code is generated or mapped.

#pragma once

#include <services/serviceman.h>

namespace skyline::service::jit {
    /**
     * @brief Diagnostic IJitEnvironment for identifying the JIT calls made by a guest.
     *
     * This is NOT an implementation of Nintendo's JIT sysmodule. Every method
     * requiring real generated code returns NotImplemented.
     */
    class IJitEnvironment : public BaseService {
      public:
        IJitEnvironment(const DeviceState &state, ServiceManager &manager);

        Result GenerateCode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result Control(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result LoadPlugin(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetCodeAddress(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0, IJitEnvironment, GenerateCode),
            SFUNC(1, IJitEnvironment, Control),
            SFUNC(1000, IJitEnvironment, LoadPlugin),
            SFUNC(1001, IJitEnvironment, GetCodeAddress)
        )
    };

    /**
     * @brief Minimal diagnostic endpoint for nn::jitsrv::IJitService (jit:u).
     */
    class IJitService : public BaseService {
      public:
        IJitService(const DeviceState &state, ServiceManager &manager);

        Result CreateJitEnvironment(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0, IJitService, CreateJitEnvironment)
        )
    };
}
