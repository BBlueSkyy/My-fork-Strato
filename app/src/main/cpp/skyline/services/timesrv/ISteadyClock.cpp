// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "core.h"
#include "ISteadyClock.h"

namespace skyline::service::timesrv {
    ISteadyClock::ISteadyClock(const DeviceState &state, ServiceManager &manager, core::SteadyClockCore &core, bool writeable, bool ignoreUninitializedChecks)
        : BaseService(state, manager),
          core(core),
          writeable(writeable),
          ignoreUninitializedChecks(ignoreUninitializedChecks) {}

    Result ISteadyClock::GetCurrentTimePoint(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!ignoreUninitializedChecks && !core.IsClockInitialized())
            return result::ClockUninitialized;

        auto timePoint{core.GetCurrentTimePoint()};
        if (timePoint)
            response.Push(*timePoint);

        return timePoint;
    }

    Result ISteadyClock::GetTestOffset(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!ignoreUninitializedChecks && !core.IsClockInitialized())
            return result::ClockUninitialized;

        response.Push(core.GetTestOffset());
        return {};
    }

    Result ISteadyClock::SetTestOffset(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!writeable)
            return result::PermissionDenied;
        if (!ignoreUninitializedChecks && !core.IsClockInitialized())
            return result::ClockUninitialized;

        core.SetTestOffset(request.Pop<TimeSpanType>());
        return {};
    }

    Result ISteadyClock::GetRtcValue(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!ignoreUninitializedChecks && !core.IsClockInitialized())
            return result::ClockUninitialized;

        auto rtcValue{core.GetRtcValue()};
        if (rtcValue)
            response.Push(*rtcValue);

        return rtcValue;
    }

    Result ISteadyClock::IsRtcResetDetected(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!ignoreUninitializedChecks && !core.IsClockInitialized())
            return result::ClockUninitialized;

        response.Push<u8>(core.IsRtcResetDetected());
        return {};
    }

    Result ISteadyClock::GetSetupResultValue(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!ignoreUninitializedChecks && !core.IsClockInitialized())
            return result::ClockUninitialized;

        response.Push(core.GetSetupResult());
        return {};
    }

    Result ISteadyClock::GetInternalOffset(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!ignoreUninitializedChecks && !core.IsClockInitialized())
            return result::ClockUninitialized;

        response.Push(core.GetInternalOffset());
        return {};
    }
}
