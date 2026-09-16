// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cxxabi.h>
#include <common/settings.h>
#include <common/trace.h>
#include "base_service.h"

namespace skyline::service {
    const std::string &BaseService::GetName() {
        if (name.empty()) {
            auto mangledName{typeid(*this).name()};

            int status{};
            size_t length{};
            std::unique_ptr<char, decltype(&std::free)> demangled{abi::__cxa_demangle(mangledName, nullptr, &length, &status), std::free};

            name = (status == 0) ? std::string(demangled.get() + std::char_traits<char>::length("skyline::service::")) : mangledName;
        }
        return name;
    }

    Result service::BaseService::HandleRequest(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        ServiceFunctionDescriptor function;
        u32 functionId{request.isTipc ? static_cast<u32>(request.header->type) : request.payload->value};

        try {
            function = GetServiceFunction(functionId, request.isTipc);
            LOGDNF("Service: {}", function.name);

            if (*state.settings->autoStub) {
                LOGI("[IPC-TRACE] service='{}' command=0x{:X} ({}) type={} handler='{}'",
                     GetName(), functionId, functionId, request.isTipc ? "TIPC" : "HIPC", function.name);

                if (std::string_view{function.name}.find("::Unsupported") != std::string_view::npos) {
                    LOGW("[AUTOSTUB][INCOMPLETE_SERVICE] service='{}' command=0x{:X} ({}) type={} handler='{}' reason=explicit-unsupported-handler",
                         GetName(), functionId, functionId, request.isTipc ? "TIPC" : "HIPC", function.name);
                }
            }
        } catch (const std::out_of_range &) {
            if (*state.settings->autoStub) {
                LOGW("[AUTOSTUB][MISSING_COMMAND] service='{}' command=0x{:X} ({}) type={} action=legacy-success-fallback",
                     GetName(), functionId, functionId, request.isTipc ? "TIPC" : "HIPC");
            } else {
                LOGW("Cannot find {0} function in service '{1}': 0x{2:X} ({2})", request.isTipc ? "TIPC" : "HIPC", GetName(), static_cast<u32>(functionId));
            }
            // Preserve Strato's existing fallback for unknown commands in implemented services.
            return {};
        }
        TRACE_EVENT("service", perfetto::StaticString{function.name});
        try {
            const Result result{function(session, request, response)};
            if (*state.settings->autoStub && result.raw != 0) {
                LOGW("[AUTOSTUB][NONZERO_RESULT] service='{}' command=0x{:X} ({}) type={} handler='{}' result=0x{:X} ({}) module={} id={}",
                     GetName(), functionId, functionId, request.isTipc ? "TIPC" : "HIPC", function.name,
                     result.raw, result.raw, result.module, result.id);
            }
            return result;
        } catch (exception &e) {
            // We need to forward any skyline::exception objects without modification even though they inherit from std::exception
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception &e) {
            throw exception("{} (Service: {})", e.what(), function.name);
        }
    }
}
