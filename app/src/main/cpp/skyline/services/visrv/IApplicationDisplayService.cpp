// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2019 Ryujinx Team and Contributors (https://github.com/Ryujinx/)

#include <gpu.h>
#include <kernel/types/KProcess.h>
#include <services/am/applet/IApplet.h>
#include <services/serviceman.h>
#include <services/hosbinder/IHOSBinderDriver.h>
#include "IApplicationDisplayService.h"
#include "ISystemDisplayService.h"
#include "IManagerDisplayService.h"
#include "indirect_layer_layout.h"
#include "indirect_layer_ipc.h"
#include "results.h"
#include <kernel/guest_boundary_trace.h>
#include <kernel/types/KThread.h>
#include <applet/swkbd/software_keyboard_trace.h>

namespace skyline::service::visrv {
    IApplicationDisplayService::IApplicationDisplayService(const DeviceState &state, ServiceManager &manager, PrivilegeLevel level) : level(level), IDisplayService(state, manager) {}

    Result IApplicationDisplayService::GetRelayService(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        manager.RegisterService(hosbinder, session, response);
        return {};
    }

    Result IApplicationDisplayService::GetIndirectDisplayTransactionService(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (level < PrivilegeLevel::System)
            return result::IllegalOperation;
        manager.RegisterService(hosbinder, session, response);
        return {};
    }

    Result IApplicationDisplayService::GetSystemDisplayService(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (level < PrivilegeLevel::System)
            return result::IllegalOperation;
        manager.RegisterService(SRVREG(ISystemDisplayService), session, response);
        return {};
    }

    Result IApplicationDisplayService::GetManagerDisplayService(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (level < PrivilegeLevel::Manager)
            return result::IllegalOperation;
        manager.RegisterService(SRVREG(IManagerDisplayService), session, response);
        return {};
    }

    Result IApplicationDisplayService::ListDisplays(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        struct DisplayInfo {
            std::array<u8, 0x40> displayName{"Default"};
            u8 hasLimitedLayers{1};
            u8 pad[7];
            u64 maxLayers{1};
            u64 width{1920};
            u64 height{1080};
        } displayInfo;

        request.outputBuf.at(0).as<DisplayInfo>() = displayInfo;
        response.Push<u64>(1);
        return {};
    }

    Result IApplicationDisplayService::OpenDisplay(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto displayName(request.PopString());
        LOGD("Opening display: {}", displayName);
        response.Push(hosbinder->OpenDisplay(displayName));
        return {};
    }

    Result IApplicationDisplayService::CloseDisplay(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto displayId{request.Pop<hosbinder::DisplayId>()};
        LOGD("Closing display: {}", hosbinder::ToString(displayId));
        hosbinder->CloseDisplay(displayId);
        return {};
    }

    Result IApplicationDisplayService::OpenLayer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto displayName(request.PopString(0x40));
        auto layerId{request.Pop<u64>()};
        LOGD("Opening layer #{} on display: {}", layerId, displayName);

        auto displayId{hosbinder->OpenDisplay(displayName)};
        auto parcel{hosbinder->OpenLayer(displayId, layerId)};
        response.Push<u64>(parcel.WriteParcel(request.outputBuf.at(0)));

        return {};
    }

    Result IApplicationDisplayService::CloseLayer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        u64 layerId{request.Pop<u64>()};
        LOGD("Closing layer #{}", layerId);
        hosbinder->CloseLayer(layerId);
        return {};
    }

    Result IApplicationDisplayService::SetLayerScalingMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto scalingMode{request.Pop<u64>()};
        auto layerId{request.Pop<u64>()};
        LOGD("Setting Layer Scaling mode to '{}' for layer {}", scalingMode, layerId);
        return {};
    }

    Result IApplicationDisplayService::GetDisplayVsyncEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        KHandle handle{state.process->InsertItem(state.gpu->presentation.vsyncEvent)};
        LOGD("V-Sync Event Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IApplicationDisplayService::ConvertScalingMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        u32 inScalingMode{request.Pop<u32>()};

        std::array<ScalingMode, 5> scalingModeLut{
            ScalingMode::None,
            ScalingMode::Freeze,
            ScalingMode::ScaleToLayer,
            ScalingMode::ScaleAndCrop,
            ScalingMode::PreserveAspectRatio,
        };

        if (scalingModeLut.size() <= inScalingMode)
            return result::InvalidArgument;

        auto scalingMode{scalingModeLut[inScalingMode]};
        if (scalingMode != ScalingMode::ScaleToLayer && scalingMode != ScalingMode::PreserveAspectRatio)
            return result::IllegalOperation;

        response.Push(scalingMode);

        return {};
    }

    Result IApplicationDisplayService::MapIndirectLayerImageRequest(ipc::IpcRequest &request, ipc::IpcResponse &response, i64 width, i64 height, u64 handle, u64 appletResourceUserId) {
        IndirectLayerLayout layout;
        if (request.outputBuf.empty()) {
            // Keep the established precedence: invalid dimensions are reported before a missing buffer.
            return CalculateIndirectLayerLayout(width, height, layout) ? result::InvalidArgument : result::InvalidDimensions;
        }

        const auto mapResult{MapIndirectLayerImage(*manager.indirectLayers, request.pid, width, height, handle, appletResourceUserId, request.outputBuf.at(0), layout)};
        if (mapResult.raw) {
            LOGW("Indirect layer image map failed: result=0x{:X}, width={}, height={}, handle=0x{:X}, aruid=0x{:X}", mapResult.raw, width, height, handle, appletResourceUserId);
            return mapResult;
        }

        WriteIndirectLayerImageMapReply(response, layout);
        return {};
    }

    Result IApplicationDisplayService::GetIndirectLayerImageMap(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto arguments{ReadIndirectLayerImageMapArguments(request)};
        return MapIndirectLayerImageRequest(request, response, arguments.width, arguments.height, arguments.handle, arguments.appletResourceUserId);
    }

    Result IApplicationDisplayService::GetIndirectLayerImageCropMap(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // The produced image is blank, so the crop rectangle cannot change its contents; the full-image size/stride are returned.
        const auto arguments{ReadIndirectLayerImageCropMapArguments(request)};
        return MapIndirectLayerImageRequest(request, response, arguments.width, arguments.height, arguments.handle, arguments.appletResourceUserId);
    }

    Result IApplicationDisplayService::GetIndirectLayerImageRequiredMemoryInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // SWKBD callers may query VI image memory before their first Calc.
        // Diagnose that boundary once per signature, without changing its result.
        using Gate = skyline::applet::swkbd::SoftwareKeyboardTraceGate;
        static Gate trace;
        if (AsyncLogger::CheckLogLevel(AsyncLogger::LogLevel::Info) &&
            trace.Take(Gate::Point::Indirect, {0, 0, 0, 0}) == Gate::Decision::Emit)
            LOGI("[SWKBD] VI GetIndirectLayerImageRequiredMemoryInfo enter");
        i64 width{request.Pop<i64>()}, height{request.Pop<i64>()};

        IndirectLayerLayout layout;
        if (!CalculateIndirectLayerLayout(width, height, layout)) {
            if (AsyncLogger::CheckLogLevel(AsyncLogger::LogLevel::Info) &&
                trace.Take(Gate::Point::Indirect, {1, static_cast<u64>(width), static_cast<u64>(height), result::InvalidDimensions.raw}) == Gate::Decision::Emit)
                LOGI("[SWKBD] VI memory-info invalid width={} height={} result=0x{:X}", width, height, result::InvalidDimensions.raw);
            return result::InvalidDimensions;
        }

        WriteIndirectLayerMemoryInfoReply(response, layout);
        if (AsyncLogger::CheckLogLevel(AsyncLogger::LogLevel::Info) &&
            trace.Take(Gate::Point::Indirect, {2, static_cast<u64>(width), static_cast<u64>(height), layout.requiredSize}) == Gate::Decision::Emit) {
            LOGI("[SWKBD] VI memory-info return width={} height={} required=0x{:X} alignment=0x{:X} result=0", width, height, layout.requiredSize, IndirectLayerAlignment);
            // Temporary: from here on record where guest threads park or which services they call (bounded, see GuestBoundaryTrace).
            if (kernel::GuestBoundaryTrace::Arm())
                LOGI("[SWKBD] guest boundary trace armed on thread #{} after VI memory-info", state.thread ? state.thread->id : 0);
        }

        return {};
    }
}
