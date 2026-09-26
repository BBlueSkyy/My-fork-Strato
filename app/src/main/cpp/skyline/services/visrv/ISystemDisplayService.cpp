// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/settings.h>
#include "ISystemDisplayService.h"

namespace skyline::service::visrv {
    ISystemDisplayService::ISystemDisplayService(const DeviceState &state, ServiceManager &manager) : IDisplayService(state, manager) {}

    Result ISystemDisplayService::SetLayerZ(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result ISystemDisplayService::GetDisplayMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto displayId{request.Pop<u64>()};
        LOGD("Getting display mode for display #{}", displayId);

        struct DisplayModeInfo {
            u32 width;
            u32 height;
            float refreshRate;
            u32 reserved;
        };
        static_assert(sizeof(DisplayModeInfo) == 0x10);

        response.Push(DisplayModeInfo{
            *state.settings->isDocked ? 1920U : 1280U,
            *state.settings->isDocked ? 1080U : 720U,
            60.0f,
            0,
        });
        return {};
    }
}
