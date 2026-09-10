// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cxxabi.h>
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
        } catch (const std::out_of_range &) {
            LOGW("Cannot find {0} function in service '{1}': 0x{2:X} ({2})", request.isTipc ? "TIPC" : "HIPC", GetName(), static_cast<u32>(functionId));
            return {};
        }

        const auto &serviceName{GetName()};
        const bool traceHid{serviceName.compare(0, 5, "hid::") == 0};
        if (traceHid) {
            LOGI("HidIpcTrace: enter service={} cmd=0x{:X} ({}) func={}",
                 serviceName, functionId, functionId, function.name);
        }

        TRACE_EVENT("service", perfetto::StaticString{function.name});
        try {
            auto result{function(session, request, response)};
            if (traceHid) {
                LOGI("HidIpcTrace: exit service={} cmd=0x{:X} ({}) func={} result=0x{:X} module={} description={}",
                     serviceName, functionId, functionId, function.name,
                     result.raw, static_cast<u32>(result.module), static_cast<u32>(result.id));
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
