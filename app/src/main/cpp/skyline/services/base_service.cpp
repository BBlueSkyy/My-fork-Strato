// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cxxabi.h>
#include <common/trace.h>
#include <kernel/types/KThread.h>
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
        TRACE_EVENT("service", perfetto::StaticString{function.name});
        try {
            auto result{function(session, request, response)};
            if (result.raw) {
                // Keep the producer visible at the normal log level. A later
                // SetTerminateResult only reports the result chosen by the
                // application; it cannot identify the IPC call that failed.
                LOGW("IPC failure: service={}, function={}, command=0x{:X}, result=0x{:08X} (module {}, description {}), args=0x{:X}, input_buffers={}, output_buffers={}",
                     GetName(), function.name, functionId, result.raw,
                     static_cast<u32>(result.module), static_cast<u32>(result.id),
                     request.cmdArgSz, request.inputBuf.size(), request.outputBuf.size());
                if (state.process->is64bit()) {
                    const auto &call{static_cast<const type::KNceThread &>(*state.thread).ctx.svcCallsite};
                    LOGW("IPC failure guest call: PC=0x{:X}, LR=0x{:X}, SP=0x{:X}", call.pc, call.lr, call.sp);
                }
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
