// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "IClkrstManager.h"
#include "IClkrstSession.h"

namespace skyline::service::clkrst {
    IClkrstSession::IClkrstSession(const DeviceState &state, ServiceManager &manager, std::shared_ptr<ClockResetState> clockState, u32 deviceCode, u32 unknown)
        : BaseService(state, manager), clockState(std::move(clockState)), deviceCode(deviceCode), unknown(unknown) {}

    Result IClkrstSession::EnableClock(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return clockState->SetClockEnabled(deviceCode, true) ? Result{} : result::InvalidArgument;
    }

    Result IClkrstSession::DisableClock(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return clockState->SetClockEnabled(deviceCode, false) ? Result{} : result::InvalidArgument;
    }

    Result IClkrstSession::AssertReset(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return clockState->SetResetAsserted(deviceCode, true) ? Result{} : result::InvalidArgument;
    }

    Result IClkrstSession::DeassertReset(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return clockState->SetResetAsserted(deviceCode, false) ? Result{} : result::InvalidArgument;
    }

    Result IClkrstSession::EnablePower(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return clockState->SetPowerEnabled(deviceCode, true) ? Result{} : result::InvalidArgument;
    }

    Result IClkrstSession::DisablePower(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return clockState->SetPowerEnabled(deviceCode, false) ? Result{} : result::InvalidArgument;
    }

    Result IClkrstSession::GetModuleState(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        const auto moduleState{clockState->GetModuleState(deviceCode)};
        if (!moduleState)
            return result::InvalidArgument;

        response.Push(*moduleState);
        return {};
    }

    Result IClkrstSession::SetClockRate(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const u32 hz{request.Pop<u32>()};
        if (!clockState->SetClockRate(deviceCode, hz))
            return result::InvalidArgument;

        LOGD("clkrst SetClockRate: device=0x{:08X}, hz={}", deviceCode, hz);
        return {};
    }

    Result IClkrstSession::GetClockRate(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        const auto hz{clockState->GetClockRate(deviceCode)};
        if (!hz)
            return result::InvalidArgument;

        response.Push<u32>(*hz);
        return {};
    }

    Result IClkrstSession::SetMinimumVoltageClockRate(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        return clockState->SetMinimumVoltageClockRate(deviceCode, request.Pop<u32>()) ? Result{} : result::InvalidArgument;
    }

    Result IClkrstSession::GetPossibleClockRates(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const i32 maxCount{request.Pop<i32>()};
        if (maxCount < 0 || !ClockResetState::IsSupportedDeviceCode(deviceCode))
            return result::InvalidArgument;

        const auto rates{clockState->GetPossibleClockRates(deviceCode)};
        const size_t bufferCount{request.outputBuf.empty() ? 0 : request.outputBuf[0].size_bytes() / sizeof(u32)};
        const size_t count{std::min({static_cast<size_t>(maxCount), rates.size(), bufferCount})};

        if (count)
            std::memcpy(request.outputBuf[0].data(), rates.data(), count * sizeof(u32));

        response.Push<i32>(static_cast<i32>(ClockRatesListType::Discrete));
        response.Push<i32>(static_cast<i32>(count));
        return {};
    }

    Result IClkrstSession::GetDvfsTable(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const i32 rateCount{request.Pop<i32>()};
        const i32 voltageCount{request.Pop<i32>()};
        if (rateCount < 0 || voltageCount < 0 || !ClockResetState::IsSupportedDeviceCode(deviceCode))
            return result::InvalidArgument;

        // Voltage rails are not emulated. Expose an empty DVFS table instead of
        // manufacturing voltage values which would not correspond to Horizon.
        response.Push<i32>(0);
        return {};
    }

    Result IClkrstSession::IsParentClock(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const u32 parentDeviceCode{request.Pop<u32>()};
        const auto isParent{clockState->IsParentClock(deviceCode, parentDeviceCode)};
        if (!isParent)
            return result::InvalidArgument;

        response.Push<bool>(*isParent);
        return {};
    }

    Result IClkrstSession::SetParentClock(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        return clockState->SetParentClock(deviceCode, request.Pop<u32>()) ? Result{} : result::InvalidArgument;
    }
}
