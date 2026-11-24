// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cxxabi.h>
#include <common/trace.h>
#include <kernel/guest_boundary_trace.h>
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
        if (kernel::GuestBoundaryTrace::IsArmed()) {
            // Temporary SWKBD diagnostic: first time each guest thread calls each service command after the VI memory query.
            const u64 thread{DeviceState::thread ? DeviceState::thread->id : 0};
            const auto object{reinterpret_cast<u64>(this) ^ (static_cast<u64>(functionId) << 48)};
            switch (kernel::GuestBoundaryTrace::Take(thread, kernel::GuestBoundaryTrace::Kind::Ipc, object)) {
                case kernel::GuestBoundaryTrace::Decision::Emit:
                    LOGI("[SWKBD] guest thread #{} IPC {} command={} tipc={}", thread, function.name, functionId, request.isTipc);
                    break;
                case kernel::GuestBoundaryTrace::Decision::Limit:
                    LOGI("[SWKBD] guest boundary trace record limit reached; disarmed");
                    break;
                default:
                    break;
            }
        }
        TRACE_EVENT("service", perfetto::StaticString{function.name});
        try {
            auto result{function(session, request, response)};
            if (result.raw)
                LOGD("{} returned 0x{:X}", function.name, result.raw);
            return result;
        } catch (exception &e) {
            // We need to forward any skyline::exception objects without modification even though they inherit from std::exception
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception &e) {
            throw exception("{} (Service: {})", e.what(), function.name);
        }
    }
}
