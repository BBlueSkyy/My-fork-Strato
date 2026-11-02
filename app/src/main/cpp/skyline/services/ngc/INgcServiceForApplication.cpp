// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "INgcServiceForApplication.h"

namespace skyline::service::ngc {
    INgcServiceForApplication::INgcServiceForApplication(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result INgcServiceForApplication::GetContentVersion(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        // NGC version is stored in the system word-filter dictionary. If the
        // dictionary is unavailable, the reference implementation returns 0.
        // This is *not* a result of checking a name for inappropriate words.
        response.Push<u32>(0);
        return {};
    }
}
