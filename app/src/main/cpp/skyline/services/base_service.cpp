// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <array>
#include <cxxabi.h>
#include <limits>
#include <mutex>
#include <string_view>
#include <kernel/types/KProcess.h>
#include <common/trace.h>
#include "base_service.h"

namespace skyline::service {
    namespace {
        // Temporary, release-compatible diagnostic for Crysis Remastered only.
        constexpr u64 CrysisProgramId{0x0100E66010ADE000ULL};
        constexpr size_t RecentIpcCapacity{48};
        constexpr size_t RecentFailureCapacity{64};

        struct RecentIpcCall {
            const char *function{};
            u32 commandId{};
            u32 result{};
            u32 repeats{1};
        };

        thread_local std::array<RecentIpcCall, RecentIpcCapacity> recentIpcCalls{};
        thread_local size_t recentIpcNext{};
        thread_local size_t recentIpcCount{};

        struct RecentFailure {
            const char *source{};
            u32 command{};
            u32 result{};
            u32 count{1};
        };

        std::mutex failureMutex;
        std::array<RecentFailure, RecentFailureCapacity> failures{};
        size_t failureNext{};
        size_t failureCount{};

        bool IsFrequentGraphicsCall(std::string_view function) {
            return function == "INvDrvServices::Ioctl" ||
                   function == "INvDrvServices::Ioctl2" ||
                   function == "IHOSBinderDriver::TransactParcel";
        }

        void RecordRecentIpcCall(const char *function, u32 commandId, u32 result) {
            // Frequent successful rendering calls otherwise overwrite all startup evidence.
            if (!result && IsFrequentGraphicsCall(function))
                return;

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

    void RecordCrysisDiagnosticFailure(const char *source, u32 command, u32 result) {
        if (!result)
            return;
        std::scoped_lock lock{failureMutex};
        // Merge matching failure identities even when separate IOCTLs interleave them.
        // This ring preserves distinct anomalies instead of filling with repeated errors.
        for (size_t i{}; i < failureCount; ++i) {
            auto &previous{failures[i]};
            if (std::string_view{previous.source} == source && previous.command == command && previous.result == result) {
                if (previous.count != std::numeric_limits<u32>::max())
                    ++previous.count;
                return;
            }
        }
        failures[failureNext] = {source, command, result, 1};
        failureNext = (failureNext + 1) % RecentFailureCapacity;
        failureCount = std::min(failureCount + 1, RecentFailureCapacity);
    }

    void DumpRecentIpcCallsForCurrentThread() {
        // Capture the shared history first; dump it before less informative successful IPCs.
        // LOGI queues messages asynchronously, which can lose the tail when a guest aborts.
        // LogSync writes and flushes each message before returning from SetTerminateResult.
        std::array<RecentFailure, RecentFailureCapacity> snapshot{};
        size_t count{};
        {
            std::scoped_lock lock{failureMutex};
            count = failureCount;
            const size_t first{(failureNext + RecentFailureCapacity - count) % RecentFailureCapacity};
            for (size_t index{}; index < count; ++index)
                snapshot[index] = failures[(first + index) % RecentFailureCapacity];
        }

        constexpr auto level{AsyncLogger::LogLevel::Info};
        AsyncLogger::LogSync(level, fmt::format(
            "CrysisFailure: last {} IPC/NVDRV/BSD errors across guest threads (oldest first)", count));
        for (size_t index{}; index < count; ++index) {
            const auto &entry{snapshot[index]};
            AsyncLogger::LogSync(level, fmt::format(
                "CrysisFailure: [{}/{}] {} cmd=0x{:X} result=0x{:X} repeat={}",
                index + 1, count, entry.source, entry.command, entry.result, entry.count));
        }

        AsyncLogger::LogSync(level, fmt::format(
            "CrysisIPC: last {} non-rendering IPC entries on aborting guest thread (oldest first)", recentIpcCount));
        const size_t start{(recentIpcNext + RecentIpcCapacity - recentIpcCount) % RecentIpcCapacity};
        for (size_t index{}; index < recentIpcCount; ++index) {
            const auto &call{recentIpcCalls[(start + index) % RecentIpcCapacity]};
            AsyncLogger::LogSync(level, fmt::format(
                "CrysisIPC: [{}/{}] {} cmd=0x{:X} result=0x{:X} repeat={}",
                index + 1, recentIpcCount, call.function, call.commandId, call.result, call.repeats));
        }
        AsyncLogger::LogSync(level, "CrysisDiag: END");
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
            if (state.process && state.process->npdm.aci0.programId == CrysisProgramId) {
                RecordRecentIpcCall(function.name, functionId, result.raw);
                if (result.raw)
                    RecordCrysisDiagnosticFailure(function.name, functionId, result.raw);
            }
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
