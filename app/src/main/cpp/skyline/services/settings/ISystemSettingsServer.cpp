// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "ISystemSettingsServer.h"
#include "ipc_helpers.h"

namespace skyline::service::settings {
    ISystemSettingsServer::ISystemSettingsServer(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result ISystemSettingsServer::GetFirmwareVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Version 1 clears revision_minor; all other bytes match Version 2.
        const SysVerTitle version{.major=9, .minor=0, .micro=0, .revMajor=4, .revMinor=0, .platform="NX", .verHash="4de65c071fd0869695b7629f75eb97b2551dbf2f", .dispVer="9.0.0", .dispTitle="NintendoSDK Firmware for NX 9.0.0-4.0"};
        return WriteBuffer(request, version, result::NullFirmwareBuffer);
    }

    Result ISystemSettingsServer::GetFirmwareVersion2(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Strato has no installed system-version archive. Preserve the existing HLE
        // profile instead of advertising an unsupported firmware revision.
        return GetFirmwareVersion(session, request, response);
    }

    Result ISystemSettingsServer::GetColorSetId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u32>(0); // Basic White
        return {};
    }
    Result ISystemSettingsServer::Unsupported(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        LOGW("Unsupported settings command: {} (TIPC={})", request.isTipc ? static_cast<u32>(request.header->type) : request.payload->value, request.isTipc);
        return result::UnknownCommand;
    }

}
