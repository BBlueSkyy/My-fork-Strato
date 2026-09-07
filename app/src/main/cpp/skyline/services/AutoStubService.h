// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "base_service.h"

namespace skyline::service {
    /**
     * @brief Generic service used by the optional Auto-Stub compatibility mode.
     * @note It intentionally returns success without fabricating output handles, objects or buffers.
     */
    class AutoStubService final : public BaseService {
      private:
        std::string serviceName;

        Result Stub(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
            u32 functionId{request.isTipc ? static_cast<u32>(request.header->type) : request.payload->value};
            LOGW("AUTO-STUB: {} command in service '{}' -> Success: 0x{:X} ({})",
                 request.isTipc ? "TIPC" : "HIPC", serviceName, functionId, functionId);
            return {};
        }

      protected:
        ServiceFunctionDescriptor GetServiceFunction(u32 id, bool isTipc) override {
            return ServiceFunctionDescriptor{
                reinterpret_cast<DerivedService *>(this),
                reinterpret_cast<decltype(ServiceFunctionDescriptor::function)>(&AutoStubService::Stub),
                "AutoStubService::Stub"
            };
        }

      public:
        AutoStubService(const DeviceState &state, ServiceManager &manager, std::string serviceName)
            : BaseService(state, manager), serviceName(std::move(serviceName)) {}
    };
}
