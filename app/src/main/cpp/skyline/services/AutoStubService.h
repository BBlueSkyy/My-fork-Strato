// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "base_service.h"

namespace skyline::service {
    /**
     * @brief Diagnostic service used when a requested service is completely missing.
     * @note It deliberately does not fabricate successful IPC replies for unknown commands.
     */
    class AutoStubService final : public BaseService {
      private:
        // CMIF unknown-method result. This is also used by explicit Unsupported handlers in the service layer.
        static constexpr Result UnknownCommand{10, 221};

        std::string serviceName;

        Result Probe(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
            const u32 functionId{request.isTipc ? static_cast<u32>(request.header->type) : request.payload->value};
            LOGW("[AUTOSTUB][MISSING_COMMAND] service='{}' command=0x{:X} ({}) type={} action=return-unknown-command",
                 serviceName, functionId, functionId, request.isTipc ? "TIPC" : "HIPC");
            return UnknownCommand;
        }

      protected:
        ServiceFunctionDescriptor GetServiceFunction(u32, bool) override {
            return ServiceFunctionDescriptor{
                reinterpret_cast<DerivedService *>(this),
                reinterpret_cast<decltype(ServiceFunctionDescriptor::function)>(&AutoStubService::Probe),
                "AutoStubService::Probe"
            };
        }

      public:
        AutoStubService(const DeviceState &state, ServiceManager &manager, std::string serviceName)
            : BaseService(state, manager), serviceName(std::move(serviceName)) {}
    };
}
