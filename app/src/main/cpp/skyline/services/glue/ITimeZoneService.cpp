// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <services/timesrv/ITimeZoneService.h>
#include "ITimeZoneService.h"

namespace skyline::service::glue {
    ITimeZoneService::ITimeZoneService(const DeviceState &state, ServiceManager &manager, std::shared_ptr<timesrv::ITimeZoneService> core, timesrv::core::TimeServiceObject &timesrvCore, bool writeable)
        : BaseService(state, manager),
          core(std::move(core)),
          timesrvCore(timesrvCore),
          writeable(writeable) {}

    Result ITimeZoneService::GetDeviceLocationName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return core->GetDeviceLocationName(session, request, response);
    }

    Result ITimeZoneService::SetDeviceLocationName(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!writeable)
            return timesrv::result::PermissionDenied;
        return core->SetDeviceLocationName(session, request, response);
    }

    Result ITimeZoneService::GetTotalLocationNameCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return core->GetTotalLocationNameCount(session, request, response);
    }

    Result ITimeZoneService::LoadLocationNameList(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return core->LoadLocationNameList(session, request, response);
    }

    Result ITimeZoneService::LoadTimeZoneRule(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return core->LoadTimeZoneRule(session, request, response);
    }

    Result ITimeZoneService::GetTimeZoneRuleVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return core->GetTimeZoneRuleVersion(session, request, response);
    }

    Result ITimeZoneService::GetDeviceLocationNameAndUpdatedTime(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return core->GetDeviceLocationNameAndUpdatedTime(session, request, response);
    }

    Result ITimeZoneService::SetDeviceLocationNameWithTimeZoneBinary(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!writeable)
            return timesrv::result::PermissionDenied;
        return core->SetDeviceLocationNameWithTimeZoneBinaryIpc(session, request, response);
    }

    Result ITimeZoneService::ParseTimeZoneBinary(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return core->ParseTimeZoneBinaryIpc(session, request, response);
    }

    Result ITimeZoneService::GetDeviceLocationNameOperationEventReadableHandle(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return core->GetDeviceLocationNameOperationEventReadableHandle(session, request, response);
    }

    Result ITimeZoneService::ToCalendarTime(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return core->ToCalendarTime(session, request, response);
    }

    Result ITimeZoneService::ToCalendarTimeWithMyRule(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return core->ToCalendarTimeWithMyRule(session, request, response);
    }

    Result ITimeZoneService::ToPosixTime(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return core->ToPosixTime(session, request, response);
    }

    Result ITimeZoneService::ToPosixTimeWithMyRule(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return core->ToPosixTimeWithMyRule(session, request, response);
    }
}
