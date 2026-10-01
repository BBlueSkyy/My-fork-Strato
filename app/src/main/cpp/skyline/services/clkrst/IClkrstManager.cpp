// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <kernel/types/KProcess.h>
#include "IClkrstSession.h"
#include "IClkrstManager.h"

namespace skyline::service::clkrst {
    IClkrstManager::IClkrstManager(const DeviceState &state, ServiceManager &manager, std::shared_ptr<ClockResetState> clockState)
        : BaseService(state, manager), clockState(std::move(clockState)) {}

    Result IClkrstManager::OpenSession(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const u32 deviceCode{request.Pop<u32>()};
        const u32 unknown{request.Pop<u32>()};

        if (!ClockResetState::IsSupportedDeviceCode(deviceCode))
            return result::InvalidArgument;

        manager.RegisterService(std::make_shared<IClkrstSession>(state, manager, clockState, deviceCode, unknown), session, response);
        return {};
    }

    Result IClkrstManager::GetTemperatureThresholds(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const i32 maxCount{request.Pop<i32>()};
        if (maxCount < 0)
            return result::InvalidArgument;

        // Strato has no emulated thermal-controller threshold table. Returning an
        // empty table preserves the IPC contract without fabricating hardware data.
        response.Push<i32>(0);
        return {};
    }

    Result IClkrstManager::NotifyTemperature(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        clockState->NotifyTemperature(request.Pop<float>());
        return {};
    }

    Result IClkrstManager::GetClkrstStateTable(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const i32 maxCount{request.Pop<i32>()};
        if (maxCount < 0)
            return result::InvalidArgument;

        const auto states{clockState->GetStateTable()};
        const size_t bufferCount{request.outputBuf.empty() ? 0 : request.outputBuf[0].size_bytes() / sizeof(ModuleState)};
        const size_t count{std::min({static_cast<size_t>(maxCount), states.size(), bufferCount})};

        if (count)
            std::memcpy(request.outputBuf[0].data(), states.data(), count * sizeof(ModuleState));

        response.Push<i32>(static_cast<i32>(count));
        return {};
    }

    Result IClkrstManager::GetClkrstStateTableUpdateEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        auto updateEvent{clockState->GetUpdateEvent()};
        response.copyHandles.push_back(state.process->InsertItem(updateEvent));
        return {};
    }

    Result IClkrstManager::GetClkrstStateTableCount(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(static_cast<u32>(ClockResetState::SupportedDeviceCodes.size()));
        return {};
    }

    Result IClkrstManager::PrintClockTree(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        LOGD("clkrst PrintClockTree requested");
        return {};
    }
}
