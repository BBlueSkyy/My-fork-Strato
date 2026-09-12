// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#include <array>
#include <kernel/types/KProcess.h>
#include "ICradleFirmwareUpdater.h"

namespace skyline::service::am {
    ICradleFirmwareUpdater::ICradleFirmwareUpdater(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager), deviceInfoChangeEvent(std::make_shared<kernel::type::KEvent>(state, false)) {}

    Result ICradleFirmwareUpdater::StartUpdate(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ICradleFirmwareUpdater::FinishUpdate(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ICradleFirmwareUpdater::GetCradleDeviceInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::array<u8, 0x10> info{};
        response.Push(info);
        return {};
    }

    Result ICradleFirmwareUpdater::GetCradleDeviceInfoChangeEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(deviceInfoChangeEvent));
        return {};
    }
}
