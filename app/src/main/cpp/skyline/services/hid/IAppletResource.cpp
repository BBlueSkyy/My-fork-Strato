// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <input.h>
#include <kernel/types/KProcess.h>
#include "IAppletResource.h"
#include "results.h"

namespace skyline::service::hid {
    IAppletResource::IAppletResource(const DeviceState &state, ServiceManager &manager, u64 aruid)
        : BaseService(state, manager), aruid{aruid} {}

    IAppletResource::~IAppletResource() {
        state.input->UnregisterAppletResource(aruid);
    }

    Result IAppletResource::GetSharedMemoryHandle(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!state.input->IsAppletResourceRegistered(aruid))
            return result::AppletResourceNotInitialized;

        auto handle{state.process->InsertItem<type::KSharedMemory>(state.input->kHid)};
        LOGD("HID Shared Memory Handle: 0x{:X}", handle);

        response.copyHandles.push_back(handle);
        return {};
    }
}
