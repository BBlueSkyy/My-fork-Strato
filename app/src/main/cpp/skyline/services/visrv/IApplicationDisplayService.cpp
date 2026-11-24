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
            if (mapResult.raw == result::InvalidValue.raw)
                LOGW("GetIndirectLayerImageMap: unknown or closed handle=0x{:X}, aruid=0x{:X}", handle, appletResourceUserId);
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
        i64 width{request.Pop<i64>()}, height{request.Pop<i64>()};

        IndirectLayerLayout layout;
        if (!CalculateIndirectLayerLayout(width, height, layout)) {
            return result::InvalidDimensions;
        }

        WriteIndirectLayerMemoryInfoReply(response, layout);
        return {};
    }
}
