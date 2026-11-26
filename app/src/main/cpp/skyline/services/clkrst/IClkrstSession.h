// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include "clock_state.h"

namespace skyline::service::clkrst {
    class IClkrstSession : public BaseService {
      private:
        std::shared_ptr<ClockResetState> clockState;
        u32 deviceCode;
        [[maybe_unused]] u32 unknown;

      public:
        IClkrstSession(const DeviceState &state, ServiceManager &manager, std::shared_ptr<ClockResetState> clockState, u32 deviceCode, u32 unknown);

        Result EnableClock(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result DisableClock(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result AssertReset(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result DeassertReset(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result EnablePower(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result DisablePower(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetModuleState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetClockRate(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetClockRate(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetMinimumVoltageClockRate(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetPossibleClockRates(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetDvfsTable(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result IsParentClock(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result SetParentClock(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, IClkrstSession, EnableClock),
            SFUNC(0x1, IClkrstSession, DisableClock),
            SFUNC(0x2, IClkrstSession, AssertReset),
            SFUNC(0x3, IClkrstSession, DeassertReset),
            SFUNC(0x4, IClkrstSession, EnablePower),
            SFUNC(0x5, IClkrstSession, DisablePower),
            SFUNC(0x6, IClkrstSession, GetModuleState),
            SFUNC(0x7, IClkrstSession, SetClockRate),
            SFUNC(0x8, IClkrstSession, GetClockRate),
            SFUNC(0x9, IClkrstSession, SetMinimumVoltageClockRate),
            SFUNC(0xA, IClkrstSession, GetPossibleClockRates),
            SFUNC(0xB, IClkrstSession, GetDvfsTable),
            SFUNC(0xC, IClkrstSession, IsParentClock),
            SFUNC(0xD, IClkrstSession, SetParentClock)
        )
    };
}
