// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cxxabi.h>
#include <common/settings.h>
#include <common/trace.h>
#include <input.h>
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

        auto isNpadDiagnosticHandler = [](std::string_view handler) {
            return handler.find("SetSupportedNpadStyleSet") != std::string_view::npos ||
                   handler.find("SetSupportedNpadIdType") != std::string_view::npos ||
                   handler.find("ActivateNpad") != std::string_view::npos ||
                   handler.find("DisconnectNpad") != std::string_view::npos ||
                   handler.find("SetNpadJoyAssignmentMode") != std::string_view::npos ||
                   handler.find("StartLrAssignmentMode") != std::string_view::npos ||
                   handler.find("StopLrAssignmentMode") != std::string_view::npos ||
                   handler.find("SetNpadHandheldActivationMode") != std::string_view::npos;
        };

        auto logNpadSnapshot = [&](std::string_view phase, std::string_view handler) {
            if (!state.input)
                return;

            auto &npad{state.input->npad};
            std::scoped_lock lock{npad.mutex};
            auto *hid{state.input->hid};

            const u32 conditionInitialized{hid ? hid->npadCondition.initialized : 0};
            const u32 conditionHoldType{hid ? hid->npadCondition.holdType : 0};
            const u32 conditionValid{hid ? hid->npadCondition.valid : 0};

            LOGI("[GRID-HID][{}] handler='{}' styles=0x{:X} supportedIds={} orientation={} handheldMode={} vibrationPermitted={} npadCondition(init={},hold={},valid={})",
                 phase, handler, npad.styles.raw, npad.supportedIds.size(),
                 static_cast<i64>(npad.orientation), static_cast<u64>(npad.handheldActivationMode),
                 npad.vibrationPermitted, conditionInitialized, conditionHoldType, conditionValid);

            for (size_t i{}; i < npad.supportedIds.size(); ++i) {
                LOGI("[GRID-HID][{}][SUPPORTED] slot={} npadId=0x{:X}",
                     phase, i, static_cast<u32>(npad.supportedIds[i]));
            }

            for (size_t i{}; i < npad.controllers.size(); ++i) {
                const auto &controller{npad.controllers[i]};
                const bool mapped{controller.device != nullptr};
                const bool connected{mapped && controller.device->connectionState.connected};
                const u32 mappedId{mapped ? static_cast<u32>(controller.device->id) : 0xFFFFFFFFU};
                const u64 connectionState{mapped ? controller.device->connectionState.raw : 0};

                LOGI("[GRID-HID][{}][HOST] slot={} type={} partner={} mapped={} npadId=0x{:X} connected={} conn=0x{:X}",
                     phase, i, static_cast<u32>(controller.type), static_cast<i32>(controller.partnerIndex),
                     mapped, mappedId, connected, connectionState);
            }

            for (size_t i{}; i < npad.npads.size(); ++i) {
                auto &device{npad.npads[i]};
                const u32 sharedType{hid ? static_cast<u32>(hid->npad[i].header.type) : 0};
                const u32 sharedAssignment{hid ? static_cast<u32>(hid->npad[i].header.assignment) : 0};
                const u32 sharedDeviceType{hid ? hid->npad[i].deviceType.raw : 0};
                const u64 sharedSystemProperties{hid ? hid->npad[i].systemProperties.raw : 0};
                const bool connected{device.connectionState.connected};

                LOGI("[GRID-HID][{}][NPAD] slot={} id=0x{:X} index={} partner={} type={} connected={} conn=0x{:X} sharedType={} sharedAssignment={} sharedDeviceType=0x{:X} sharedSystemProperties=0x{:X}",
                     phase, i, static_cast<u32>(device.id), static_cast<i32>(device.index),
                     static_cast<i32>(device.partnerIndex), static_cast<u32>(device.type),
                     connected, device.connectionState.raw,
                     sharedType, sharedAssignment, sharedDeviceType, sharedSystemProperties);
            }
        };

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
            const bool traceNpad{*state.settings->autoStub && GetName() == "hid::IHidServer" && isNpadDiagnosticHandler(function.name)};
            if (traceNpad)
                logNpadSnapshot("BEFORE", function.name);

            auto result{function(session, request, response)};

            if (traceNpad)
                logNpadSnapshot("AFTER", function.name);

            return result;
        } catch (exception &e) {
            // We need to forward any skyline::exception objects without modification even though they inherit from std::exception
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception &e) {
            throw exception("{} (Service: {})", e.what(), function.name);
        }
    }
}
