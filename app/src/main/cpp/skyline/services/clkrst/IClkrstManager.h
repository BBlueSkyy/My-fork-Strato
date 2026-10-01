// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include "clock_state.h"

namespace skyline::service::clkrst {
    namespace result {
        constexpr Result InvalidArgument(30, 5);
    }

    class IClkrstManager : public BaseService {
      private:
        std::shared_ptr<ClockResetState> clockState;

      public:
        IClkrstManager(const DeviceState &state, ServiceManager &manager, std::shared_ptr<ClockResetState> clockState);

        Result OpenSession(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetTemperatureThresholds(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result NotifyTemperature(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetClkrstStateTable(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetClkrstStateTableUpdateEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result GetClkrstStateTableCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
        Result PrintClockTree(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, IClkrstManager, OpenSession),
            SFUNC(0x1, IClkrstManager, GetTemperatureThresholds),
            SFUNC(0x2, IClkrstManager, NotifyTemperature),
            SFUNC(0x3, IClkrstManager, GetClkrstStateTable),
            SFUNC(0x4, IClkrstManager, GetClkrstStateTableUpdateEvent),
            SFUNC(0x5, IClkrstManager, GetClkrstStateTableCount),
            SFUNC(0x6, IClkrstManager, PrintClockTree)
        )
    };
}
