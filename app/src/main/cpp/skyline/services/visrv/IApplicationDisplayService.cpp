// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2019 Ryujinx Team and Contributors (https://github.com/Ryujinx/)

#include <gpu.h>
#include <kernel/svc.h>
#include <kernel/types/KProcess.h>
#include <services/am/applet/IApplet.h>
#include <services/serviceman.h>
#include <services/hosbinder/IHOSBinderDriver.h>
#include "IApplicationDisplayService.h"
#include "ISystemDisplayService.h"
#include "IManagerDisplayService.h"
#include "indirect_layer_layout.h"
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

    Result IApplicationDisplayService::GetIndirectLayerImageMap(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto width{request.Pop<i64>()};
        auto height{request.Pop<i64>()};
        const auto handle{request.Pop<u64>()};
        const auto appletResourceUserId{request.Pop<u64>()};
        LOGI("GetIndirectLayerImageMap: entered, pid=0x{:X}, handle=0x{:X}, ARUID=0x{:X}, width={}, height={}",
             request.pid, handle, appletResourceUserId, width, height);
        IndirectLayerLayout layout;
        if (!CalculateIndirectLayerLayout(width, height, layout)) {
            LOGI("GetIndirectLayerImageMap: return InvalidDimensions");
            return result::InvalidDimensions;
        }
        if (request.outputBuf.empty()) {
            LOGI("GetIndirectLayerImageMap: return InvalidArgument, missing output buffer");
            return result::InvalidArgument;
        }

        auto imageBuffer{request.outputBuf.at(0)};
        if (imageBuffer.size() < layout.imageSize || reinterpret_cast<uintptr_t>(imageBuffer.data()) % IndirectLayerAlignment) {
            LOGI("GetIndirectLayerImageMap: return InvalidArgument, bufferSize=0x{:X}, required=0x{:X}, aligned={}",
                 imageBuffer.size(), layout.imageSize, reinterpret_cast<uintptr_t>(imageBuffer.data()) % IndirectLayerAlignment == 0);
            return result::InvalidArgument;
        }

        const auto applet{manager.indirectLayers->Get(handle, request.pid, appletResourceUserId)};
        if (!applet) {
            LOGW("GetIndirectLayerImageMap: unknown or closed handle=0x{:X}, aruid=0x{:X}", handle, appletResourceUserId);
            LOGI("GetIndirectLayerImageMap: return InvalidValue");
            return result::InvalidValue;
        }

        const bool available{applet->GetIndirectLayerImage(imageBuffer.first(layout.imageSize))};
        LOGD("GetIndirectLayerImageMap: handle=0x{:X}, aruid=0x{:X}, width={}, height={}, size=0x{:X}, available={}",
             handle, appletResourceUserId, width, height, layout.imageSize, available);
        if (!available) {
            LOGI("GetIndirectLayerImageMap: return NoData");
            return result::NoData;
        }

        response.Push<i64>(static_cast<i64>(layout.imageSize));
        response.Push<i64>(static_cast<i64>(layout.stride));
        LOGI("GetIndirectLayerImageMap: return Success, size=0x{:X}, pitch=0x{:X}", layout.imageSize, layout.stride);

        return {};
    }

    Result IApplicationDisplayService::GetIndirectLayerImageRequiredMemoryInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i64 width{request.Pop<i64>()}, height{request.Pop<i64>()};
        LOGI("GetIndirectLayerImageRequiredMemoryInfo: entered, pid=0x{:X}, width={}, height={}", request.pid, width, height);

        IndirectLayerLayout layout;
        if (!CalculateIndirectLayerLayout(width, height, layout)) {
            LOGI("GetIndirectLayerImageRequiredMemoryInfo: return InvalidDimensions");
            return result::InvalidDimensions;
        }

        response.Push<i64>(static_cast<i64>(layout.requiredSize));
        response.Push<i64>(static_cast<i64>(IndirectLayerAlignment));
        LOGI("GetIndirectLayerImageRequiredMemoryInfo: return Success, size=0x{:X}, alignment=0x{:X}",
             layout.requiredSize, IndirectLayerAlignment);
        // Diagnostic branch: capture the immediate guest-kernel activity after cmd2460.
        kernel::svc::TraceNextSvcs(state.thread->id, 8);

        return {};
    }
}
