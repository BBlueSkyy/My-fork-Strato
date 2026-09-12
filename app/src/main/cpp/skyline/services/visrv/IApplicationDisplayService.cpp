// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2019 Ryujinx Team and Contributors (https://github.com/Ryujinx/)

#include <gpu.h>
#include <limits>
#include <kernel/types/KProcess.h>
#include <services/am/applet/IApplet.h>
#include <services/serviceman.h>
#include <services/hosbinder/IHOSBinderDriver.h>
#include "IApplicationDisplayService.h"
#include "ISystemDisplayService.h"
#include "IManagerDisplayService.h"
#include "results.h"

namespace skyline::service::visrv {
    namespace {
        constexpr u64 IndirectLayerAlignment{0x1000};

        bool GetIndirectLayerSize(i64 width, i64 height, u64 &pitch, u64 &size) {
            constexpr u64 BlockSize{0x20000};
            constexpr u64 MaxSize{std::numeric_limits<i64>::max()};
            if (width <= 0 || height <= 0 || static_cast<u64>(width) > (MaxSize - 63) / 4 ||
                static_cast<u64>(height) > MaxSize - 63)
                return false;

            pitch = util::AlignUpNpot<u64>(static_cast<u64>(width) * 4, 64);
            const u64 alignedHeight{util::AlignUpNpot<u64>(static_cast<u64>(height), 64)};
            if (alignedHeight > (MaxSize - (BlockSize - 1)) / pitch)
                return false;

            size = util::AlignUpNpot<u64>(pitch * alignedHeight, BlockSize);
            return true;
        }
    }

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
        u64 pitch{}, size{};
        if (!GetIndirectLayerSize(width, height, pitch, size))
            return result::InvalidDimensions;
        if (request.outputBuf.empty())
            return result::InvalidArgument;

        auto imageBuffer{request.outputBuf.at(0)};
        if (imageBuffer.size() < size || reinterpret_cast<uintptr_t>(imageBuffer.data()) % IndirectLayerAlignment)
            return result::InvalidArgument;

        const auto applet{manager.indirectLayers->Get(handle)};
        if (!applet) {
            LOGW("GetIndirectLayerImageMap: unknown or closed handle=0x{:X}, aruid=0x{:X}", handle, appletResourceUserId);
            return result::InvalidValue;
        }

        const bool available{applet->GetIndirectLayerImage(imageBuffer.first(size))};
        LOGD("GetIndirectLayerImageMap: handle=0x{:X}, aruid=0x{:X}, width={}, height={}, size=0x{:X}, available={}",
             handle, appletResourceUserId, width, height, size, available);
        if (!available)
            return result::NoData;

        response.Push<i64>(width);
        response.Push<i64>(height);

        return {};
    }

    Result IApplicationDisplayService::GetIndirectLayerImageRequiredMemoryInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i64 width{request.Pop<i64>()}, height{request.Pop<i64>()};

        u64 pitch{}, size{};
        if (!GetIndirectLayerSize(width, height, pitch, size))
            return result::InvalidDimensions;

        LOGI("GetIndirectLayerImageRequiredMemoryInfo: width={}, height={}, pitch=0x{:X}, size=0x{:X}, alignment=0x{:X}",
             width, height, pitch, size, IndirectLayerAlignment);

        response.Push<i64>(size);
        response.Push<u64>(IndirectLayerAlignment);

        return {};
    }
}
