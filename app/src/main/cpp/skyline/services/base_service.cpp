// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <array>
#include <cxxabi.h>
#include <limits>
#include <kernel/types/KProcess.h>
#include <common/trace.h>
#include "base_service.h"

namespace skyline::service {
    namespace {
        // Temporary, release-compatible diagnostic for Crysis Remastered only.
        constexpr u64 CrysisProgramId{0x0100E66010ADE000ULL};
        constexpr size_t RecentIpcCapacity{48};

        struct RecentIpcCall {
            const char *function{};
            u32 commandId{};
            u32 result{};
            u32 repeats{1};
        };

        thread_local std::array<RecentIpcCall, RecentIpcCapacity> recentIpcCalls{};
        thread_local size_t recentIpcNext{};
        thread_local size_t recentIpcCount{};

        void RecordRecentIpcCall(const char *function, u32 commandId, u32 result) {
            if (recentIpcCount) {
                auto &previous{recentIpcCalls[(recentIpcNext + RecentIpcCapacity - 1) % RecentIpcCapacity]};
                if (previous.function == function && previous.commandId == commandId && previous.result == result) {
                    if (previous.repeats != std::numeric_limits<u32>::max())
                        ++previous.repeats;
                    return;
                }
            }

            recentIpcCalls[recentIpcNext] = {function, commandId, result, 1};
            recentIpcNext = (recentIpcNext + 1) % RecentIpcCapacity;
            recentIpcCount = std::min(recentIpcCount + 1, RecentIpcCapacity);
        }
    }

    void DumpRecentIpcCallsForCurrentThread() {
        LOGI("CrysisIPC: last {} distinct IPC calls on aborting guest thread (oldest first)", recentIpcCount);
        const size_t start{(recentIpcNext + RecentIpcCapacity - recentIpcCount) % RecentIpcCapacity};
        for (size_t index{}; index < recentIpcCount; ++index) {
            const auto &call{recentIpcCalls[(start + index) % RecentIpcCapacity]};
            LOGI("CrysisIPC: [{}/{}] {} cmd=0x{:X} result=0x{:X} repeat={}",
                 index + 1, recentIpcCount, call.function, call.commandId, call.result, call.repeats);
        }
    }

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
            if (state.process && state.process->npdm.aci0.programId == CrysisProgramId)
                RecordRecentIpcCall(function.name, functionId, result.raw);
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
